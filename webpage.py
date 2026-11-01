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
        "mode": None,
        "configured_setpoint": None,
        "adc_voltage_mv": None,
        "adc_current_mv": None,
        "medido": 0.0,
        "pwm": 0.0,
        "v_max_cfg": 15.0,
        "i_max_cfg": 0.5,
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


def send_command_and_wait(command, expected_response, timeout_s=2.0):
    """Envía un comando y espera el ACK correspondiente de la ESP32.

    Mientras llega la respuesta, conserva la telemetría recibida en el historial.
    """
    if not send_command(command):
        return False, "No hay una conexión UART abierta."

    deadline = time.monotonic() + timeout_s
    rx_queue = st.session_state.rx_queue
    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        try:
            message = rx_queue.get(timeout=min(0.1, max(remaining, 0.0)))
        except queue.Empty:
            continue

        message_type = message.get("type")
        if message_type == "telemetry":
            update_telemetry(message)
        elif message_type == "response":
            response = message.get("text", "")
            st.session_state.last_response = response
            if response == expected_response:
                return True, response
            if response.startswith("ERR"):
                return False, response
        elif message_type == "error":
            st.session_state.last_response = message.get("text", "")
            return False, st.session_state.last_response

    return False, f"Sin respuesta {expected_response} de la ESP32 (tiempo agotado)."


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
    target_current = float(data.get("target_current_a", 0.0))
    dac_output = int(data.get("dac_output", 0))
    adc_voltage_mv = data.get("adc_voltage_mv")
    adc_current_mv = data.get("adc_current_mv")
    st.session_state.current_data.update(
        {
            "v": voltage,
            "i": current,
            "p": voltage * current,
            "setpoint": target_current,
            "mode": data.get("mode"),
            "configured_setpoint": data.get("setpoint"),
            "adc_voltage_mv": float(adc_voltage_mv) if adc_voltage_mv is not None else None,
            "adc_current_mv": float(adc_current_mv) if adc_current_mv is not None else None,
            "medido": current,
            "pwm": dac_output,
        }
    )
    st.session_state.history.append(
        {
            "Tiempo": data.get("timestamp_us", time.time() * 1_000_000) / 1_000_000,
            "Objetivo (A)": target_current,
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

active_mode = st.session_state.current_data.get("mode")
active_setpoint = st.session_state.current_data.get("configured_setpoint")
if active_mode == "CR":
    active_setpoint_text = f"{active_setpoint:.2f} Ω" if active_setpoint is not None else "—"
elif active_mode == "CC":
    active_setpoint_text = f"{active_setpoint:.3f} A" if active_setpoint is not None else "—"
else:
    active_setpoint_text = "—"
if active_mode in ("CC", "CR"):
    st.info(
        f"Estado informado por ESP32: **{active_mode}** · "
        f"Consigna: **{active_setpoint_text}** · "
        f"Corriente objetivo calculada: **{st.session_state.current_data['setpoint']:.3f} A**"
    )
else:
    st.caption("Esperando que la ESP32 envíe modo y consigna activos por UART.")

st.divider()
col_ctrl, col_lim = st.columns(2)

with st.expander("Diagnóstico de calibración ADC"):
    adc_voltage_mv = st.session_state.current_data["adc_voltage_mv"]
    adc_current_mv = st.session_state.current_data["adc_current_mv"]
    if adc_voltage_mv is None or adc_current_mv is None:
        st.caption("El firmware actual aún no envía las lecturas ADC de diagnóstico.")
    else:
        adc_col1, adc_col2 = st.columns(2)
        adc_col1.metric("ADC VIN · GPIO1", f"{adc_voltage_mv:.0f} mV")
        adc_col2.metric("ADC VSENS · GPIO2", f"{adc_current_mv:.0f} mV")
        st.caption("Lecturas calibradas y filtradas que se usan como entrada de las tablas de escala.")

with col_ctrl:
    st.subheader("Control del lazo")
    modo = st.radio("Modo a enviar", ["Corriente constante (CC)", "Resistencia constante (CR)"])
    modo_str = "CC" if "Corriente" in modo else "CR"
    unidad = "A" if modo_str == "CC" else "Ω"
    minimum = 0.0 if modo_str == "CC" else 1.0
    with st.form("control_carga"):
        setpoint = st.number_input(
            f"Consigna a enviar ({unidad})",
            min_value=minimum,
            max_value=20.0,
            value=minimum,
            step=0.1,
            format="%.3f" if modo_str == "CC" else "%.1f",
            key=f"setpoint_{modo_str.lower()}",
        )
        iniciar = st.form_submit_button("Iniciar carga", type="primary", use_container_width=True)

    if modo_str == "CR" and setpoint > 0.0:
        estimated_current = st.session_state.current_data["v"] / setpoint
        if estimated_current > 0.20:
            st.warning(
                f"Con los {st.session_state.current_data['v']:.2f} V que informa la ESP32 "
                f"y {setpoint:.2f} Ω, V/R estima {estimated_current:.2f} A. "
                "El informe solo documenta "
                "calibración de corriente hasta 0.20 A; verifica el límite real del "
                "circuito antes de enviar esta consigna."
            )

    if iniciar:
        if modo_str == "CR" and setpoint < 1.0:
            st.error("En modo CR la resistencia debe ser de al menos 1 Ω.")
        else:
            mode_ok, mode_result = send_command_and_wait(f"MODE {modo_str}", "OK MODE")
            if not mode_ok:
                st.error(f"La ESP32 no confirmó el modo {modo_str}: {mode_result}")
            else:
                setpoint_ok, setpoint_result = send_command_and_wait(
                    f"SETPOINT {setpoint:.6g}", "OK SETPOINT"
                )
                if not setpoint_ok:
                    st.error(f"La ESP32 no confirmó la consigna: {setpoint_result}")
                else:
                    arm_ok, arm_result = send_command_and_wait("ARM", "OK ARM")
                    if arm_ok:
                        st.success(f"Carga iniciada: {modo_str} @ {setpoint:.6g} {unidad}")
                    else:
                        st.error(f"Modo y consigna enviados, pero no se confirmó el inicio: {arm_result}")

    if st.button("Detener carga", use_container_width=True):
        stop_ok, stop_result = send_command_and_wait("STOP", "OK STOP")
        if stop_ok:
            st.success("Carga detenida")
        else:
            st.error(f"No se confirmó la parada: {stop_result}")

with col_lim:
    st.subheader("Límites de protección")
    limits = st.session_state.current_data
    st.info(
        f"Máx V: **{limits['v_max_cfg']:.2f} V** | "
        f"Máx I: **{limits['i_max_cfg']:.2f} A**"
    )
    st.caption("Configura el modo y la consigna, y luego pulsa «Iniciar carga». La carga no cambia mientras editas el formulario.")
    with st.expander("Modificar límites"):
        new_v_max = st.number_input("Nuevo voltaje máximo (V)", min_value=1.0, max_value=50.0, value=limits["v_max_cfg"])
        new_i_max = st.number_input("Nueva corriente máxima (A)", min_value=0.1, max_value=15.0, value=limits["i_max_cfg"])
        if st.button("Aplicar límites de seguridad"):
            voltage_sent, voltage_result = send_command_and_wait(
                f"OVP {new_v_max:.6g}", "OK OVP"
            )
            current_sent, current_result = (False, "No enviado")
            if voltage_sent:
                current_sent, current_result = send_command_and_wait(
                    f"OCP {new_i_max:.6g}", "OK OCP"
                )
            if voltage_sent and current_sent:
                st.session_state.current_data["v_max_cfg"] = new_v_max
                st.session_state.current_data["i_max_cfg"] = new_i_max
                st.success("Límites confirmados por la ESP32")
            elif not voltage_sent:
                st.error(f"La ESP32 no confirmó OVP: {voltage_result}")
            else:
                st.error(f"La ESP32 no confirmó OCP: {current_result}")

st.divider()
st.subheader("Monitoreo del PID")
if st.session_state.history:
    chart = pd.DataFrame(st.session_state.history).set_index("Tiempo")
    st.markdown("**Corriente objetivo (A)**")
    st.line_chart(chart[["Objetivo (A)"]])
    st.markdown("**Corriente medida (A)**")
    st.line_chart(chart[["Medido"]])
    st.markdown("**Salida del DAC (cuentas, 0–4095)**")
    st.line_chart(chart[["Salida DAC"]])
else:
    st.caption("Esperando telemetría JSON de la UART...")

time.sleep(0.5)
st.rerun()
