import json
import os
import queue
import select
import threading
import time
from collections import deque

import pandas as pd
import streamlit as st


st.set_page_config(page_title="Control de Carga Electrónica", layout="wide")

if "link_fd" not in st.session_state:
    st.session_state.link_fd = None
    st.session_state.reader_thread = None
    st.session_state.stop_event = None
    st.session_state.rx_queue = queue.Queue(maxsize=512)
    st.session_state.current_data = {
        "v": 0.0,
        "i": 0.0,
        "p": 0.0,
        "setpoint": 0.0,
        "medido": 0.0,
        "pwm": 0.0,
        "v_max_cfg": 30.0,
        "i_max_cfg": 5.0,
    }
    st.session_state.history = deque(maxlen=60)
    st.session_state.last_response = ""


def serial_reader(fd, rx_queue, stop_event):
    """Lee lineas del dispositivo /dev/esp32_link y las pasa al hilo Streamlit."""
    pending = bytearray()
    while not stop_event.is_set():
        try:
            readable, _, _ = select.select([fd], [], [], 0.2)
            if not readable:
                continue
            chunk = os.read(fd, 512)
            if not chunk:
                continue
            pending.extend(chunk)
            while b"\n" in pending:
                raw_line, _, remainder = pending.partition(b"\n")
                pending = bytearray(remainder)
                line = raw_line.decode("utf-8", errors="replace").strip()
                if not line:
                    continue
                try:
                    message = json.loads(line)
                except json.JSONDecodeError:
                    message = {"type": "response", "text": line}
                try:
                    rx_queue.put_nowait(message)
                except queue.Full:
                    try:
                        rx_queue.get_nowait()
                    except queue.Empty:
                        pass
                    rx_queue.put_nowait(message)
        except (OSError, ValueError):
            if not stop_event.is_set():
                try:
                    rx_queue.put_nowait({"type": "error", "text": "Se perdió el acceso al UART."})
                except queue.Full:
                    pass
            break


def send_command(command):
    """Envía una línea del protocolo ASCII que interpreta uart_bridge.c."""
    fd = st.session_state.link_fd
    if fd is None:
        return False
    payload = (command + "\n").encode("ascii")
    sent = 0
    try:
        while sent < len(payload):
            sent += os.write(fd, payload[sent:])
    except OSError as error:
        st.session_state.last_response = f"Error UART: {error}"
        return False
    return True


def disconnect():
    stop_event = st.session_state.stop_event
    if stop_event is not None:
        stop_event.set()
    fd = st.session_state.link_fd
    st.session_state.link_fd = None
    if fd is not None:
        try:
            os.close(fd)
        except OSError:
            pass
    thread = st.session_state.reader_thread
    if thread is not None and thread.is_alive():
        thread.join(timeout=0.5)
    st.session_state.reader_thread = None
    st.session_state.stop_event = None


def update_telemetry(data):
    voltage = float(data.get("voltage_v", 0.0))
    current = float(data.get("current_a", 0.0))
    setpoint = float(data.get("target_current_a", 0.0))
    dac_output = int(data.get("dac_output", 0))
    st.session_state.current_data.update(
        {
            "v": voltage,
            "i": current,
            "p": voltage * current,
            "setpoint": setpoint,
            "medido": current,
            "pwm": dac_output,
        }
    )
    st.session_state.history.append(
        {
            "Tiempo": data.get("timestamp_us", time.time() * 1_000_000) / 1_000_000,
            "Setpoint": setpoint,
            "Medido": current,
            "Salida DAC": dac_output,
        }
    )


# Aplicar en el hilo principal los mensajes acumulados por el lector.
while True:
    try:
        message = st.session_state.rx_queue.get_nowait()
    except queue.Empty:
        break
    if message.get("type") == "telemetry":
        update_telemetry(message)
    elif message.get("type") == "response":
        st.session_state.last_response = message.get("text", "")
    elif message.get("type") == "error":
        st.session_state.last_response = message.get("text", "")


st.sidebar.title("Conexión RPi - ESP32")
port = st.sidebar.text_input("Dispositivo", "/dev/esp32_link")
st.sidebar.caption("UART configurada a 115200, 8N1 por el driver del kernel.")

if st.sidebar.button("Conectar / Desconectar"):
    if st.session_state.link_fd is None:
        try:
            fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
            stop_event = threading.Event()
            rx_queue = queue.Queue(maxsize=512)
            reader_thread = threading.Thread(
                target=serial_reader,
                args=(fd, rx_queue, stop_event),
                daemon=True,
            )
            st.session_state.link_fd = fd
            st.session_state.stop_event = stop_event
            st.session_state.rx_queue = rx_queue
            st.session_state.reader_thread = reader_thread
            reader_thread.start()
            st.sidebar.success("Conectado al ESP32")
        except OSError as error:
            st.sidebar.error(f"Error abriendo {port}: {error}")
    else:
        disconnect()
        st.sidebar.warning("Desconectado")

if st.session_state.last_response:
    st.sidebar.caption(f"ESP32: {st.session_state.last_response}")

st.title("⚡ Dashboard de Carga Electrónica")
st.subheader("Mediciones actuales")
col1, col2, col3 = st.columns(3)
col1.metric("Tensión (V)", f"{st.session_state.current_data['v']:.3f} V")
col2.metric("Corriente (A)", f"{st.session_state.current_data['i']:.3f} A")
col3.metric("Potencia (W)", f"{st.session_state.current_data['p']:.2f} W")

st.divider()
col_ctrl, col_lim = st.columns(2)

with col_ctrl:
    st.subheader("Control del lazo")
    modo = st.radio("Modo de operación", ["Corriente constante (CC)", "Resistencia constante (CR)"])
    setpoint = st.number_input("Setpoint", min_value=0.0, max_value=20.0, step=0.1)

    if st.button("Enviar setpoint"):
        modo_str = "CC" if "Corriente" in modo else "CR"
        if send_command(f"MODE {modo_str}") and send_command(f"SETPOINT {setpoint:.6g}"):
            st.toast(f"Comando enviado: {modo_str} @ {setpoint:.6g}")
        else:
            st.error("No se pudo enviar el comando. Comprueba la conexión UART.")

with col_lim:
    st.subheader("Límites de protección")
    limits = st.session_state.current_data
    st.info(
        f"Máx V: **{limits['v_max_cfg']:.2f} V** | "
        f"Máx I: **{limits['i_max_cfg']:.2f} A**"
    )
    with st.expander("Modificar límites"):
        new_v_max = st.number_input("Nuevo voltaje máximo (V)", min_value=1.0, max_value=50.0, value=limits["v_max_cfg"])
        new_i_max = st.number_input("Nueva corriente máxima (A)", min_value=0.1, max_value=15.0, value=limits["i_max_cfg"])
        if st.button("Aplicar límites de seguridad"):
            voltage_sent = send_command(f"OVP {new_v_max:.6g}")
            current_sent = send_command(f"OCP {new_i_max:.6g}")
            if voltage_sent and current_sent:
                st.session_state.current_data["v_max_cfg"] = new_v_max
                st.session_state.current_data["i_max_cfg"] = new_i_max
                st.toast("Límites enviados")
            else:
                st.error("No se pudieron enviar los límites. Comprueba la conexión UART.")

st.divider()
st.subheader("Monitoreo del PID")
if st.session_state.history:
    chart = pd.DataFrame(st.session_state.history).set_index("Tiempo")
    st.line_chart(chart[["Setpoint", "Medido", "Salida DAC"]])
else:
    st.caption("Esperando telemetría JSON de la UART...")

time.sleep(0.5)
st.rerun()
