import streamlit as st
import serial
import threading
import time
import json
import pandas as pd
from collections import deque

# --- CONFIGURACIÓN DE PÁGINA ---
st.set_page_config(page_title="Control de Carga Electrónica", layout="wide")

# --- INICIALIZACIÓN DEL ESTADO GLOBAL ---
# Streamlit recarga el script en cada interacción. session_state mantiene los datos vivos.
if 'serial_conn' not in st.session_state:
    st.session_state.serial_conn = None
    st.session_state.is_reading = False
    # Estado inicial de las mediciones y variables
    st.session_state.current_data = {
        "v": 0.0, "i": 0.0, "p": 0.0, 
        "setpoint": 0.0, "medido": 0.0, "pwm": 0.0,
        "v_max_cfg": 30.0, "i_max_cfg": 5.0
    }
    # Histórico de datos para trazar la curva del PID
    st.session_state.history = deque(maxlen=60) 

# --- HILO DE LECTURA UART ---
def serial_reader():
    """Lee continuamente la telemetría del ESP32 sin bloquear la web."""
    while st.session_state.is_reading and st.session_state.serial_conn:
        try:
            if st.session_state.serial_conn.in_waiting > 0:
                line = st.session_state.serial_conn.readline().decode('utf-8').strip()
                data = json.loads(line)
                
                st.session_state.current_data.update(data)
                
                # Almacenar puntos para el gráfico del lazo de control
                st.session_state.history.append({
                    "Tiempo": time.time(),
                    "Setpoint": data.get("setpoint", 0.0),
                    "Medido": data.get("i", 0.0), # Corriente si está en CC
                    "Salida PWM": data.get("pwm", 0.0) 
                })
        except Exception:
            pass # Ignorar tramas corruptas
        time.sleep(0.05)

# --- FUNCIÓN DE ESCRITURA UART ---
def send_command(cmd_dict):
    """Envía comandos en formato JSON a la cola de FreeRTOS en el ESP32."""
    if st.session_state.serial_conn and st.session_state.serial_conn.is_open:
        msg = json.dumps(cmd_dict) + '\n'
        st.session_state.serial_conn.write(msg.encode('utf-8'))

# --- BARRA LATERAL: CONEXIÓN ---
st.sidebar.title("Conexión RPi - ESP32")
port = st.sidebar.text_input("Puerto UART", "/dev/ttyS0")
baud = st.sidebar.selectbox("Baudrate", [9600, 115200], index=1)

if st.sidebar.button("Conectar / Desconectar"):
    if st.session_state.serial_conn is None:
        try:
            st.session_state.serial_conn = serial.Serial(port, baud, timeout=1)
            st.session_state.is_reading = True
            threading.Thread(target=serial_reader, daemon=True).start()
            st.sidebar.success("Conectado al ESP32")
        except Exception as e:
            st.sidebar.error(f"Error abriendo puerto: {e}")
    else:
        st.session_state.is_reading = False
        st.session_state.serial_conn.close()
        st.session_state.serial_conn = None
        st.sidebar.warning("Desconectado")

# --- PANEL PRINCIPAL ---
st.title("⚡ Dashboard de Carga Electrónica")

# 1. Telemetría en tiempo real
st.subheader("Mediciones Actuales")
col1, col2, col3 = st.columns(3)
col1.metric("Tensión (V)", f"{st.session_state.current_data['v']:.3f} V")
col2.metric("Corriente (A)", f"{st.session_state.current_data['i']:.3f} A")
col3.metric("Potencia (W)", f"{st.session_state.current_data['p']:.2f} W")

st.divider()

# 2. Controles Principales y Límites
col_ctrl, col_lim = st.columns(2)

with col_ctrl:
    st.subheader("Control del Lazo")
    modo = st.radio("Modo de Operación", ["Corriente Constante (CC)", "Resistencia Constante (CR)"])
    setpoint = st.number_input("Setpoint", min_value=0.0, max_value=20.0, step=0.1)
    
    if st.button("Enviar Setpoint"):
        modo_str = "CC" if "Corriente" in modo else "CR"
        send_command({"cmd": "set_mode", "mode": modo_str})
        send_command({"cmd": "set_sp", "val": setpoint})
        st.toast(f"Comando enviado: {modo_str} @ {setpoint}")

with col_lim:
    st.subheader("Límites de Protección")
    # Indicador simple de los límites activos guardados en el microcontrolador
    st.info(f"🛑 Límites Activos leídos del hardware -> Máx V: **{st.session_state.current_data['v_max_cfg']} V** | Máx I: **{st.session_state.current_data['i_max_cfg']} A**")
    
    with st.expander("Modificar Límites"):
        new_v_max = st.number_input("Nuevo Voltaje Máximo (V)", min_value=1.0, max_value=50.0, value=30.0)
        new_i_max = st.number_input("Nueva Corriente Máxima (A)", min_value=0.1, max_value=15.0, value=5.0)
        if st.button("Aplicar Límites de Seguridad"):
            send_command({"cmd": "set_limits", "v_max": new_v_max, "i_max": new_i_max})
            st.toast("Límites actualizados")

st.divider()

# 3. Gráfico del comportamiento del PID
st.subheader("Monitoreo del PID")
if len(st.session_state.history) > 0:
    df = pd.DataFrame(st.session_state.history)
    df = df.set_index("Tiempo")
    # Trazar las 3 variables: Setpoint deseado, valor medido real y esfuerzo de control (PWM)
    st.line_chart(df[["Setpoint", "Medido", "Salida PWM"]])
else:
    st.caption("Esperando datos de telemetría de la UART...")

# --- AUTO-REFRESH ---
# Obliga a Streamlit a redibujar la interfaz para reflejar los datos nuevos del hilo UART
time.sleep(1)
st.rerun()