import json
import os
import sqlite3
from datetime import datetime, timezone

import paho.mqtt.client as mqtt
from dotenv import load_dotenv
from supabase import create_client, Client
from google import genai


# =========================
# Configuración general
# =========================

load_dotenv()

DB_PATH = "air_quality.db"

MQTT_HOST = "localhost"
MQTT_PORT = 1883
MQTT_TOPIC = "sabana/aire/nodo1"

ALARM_COMMANDS_TABLE = "alarm_commands"
MQTT_CMD_TOPIC = "sabana/aire/nodo1/cmd"


UBIDOTS_MQTT_ENABLED = os.getenv("UBIDOTS_MQTT_ENABLED", "false").lower() == "true"
UBIDOTS_MQTT_HOST = os.getenv("UBIDOTS_MQTT_HOST", "industrial.api.ubidots.com")
UBIDOTS_MQTT_PORT = int(os.getenv("UBIDOTS_MQTT_PORT", "1883"))
UBIDOTS_TOKEN = os.getenv("UBIDOTS_TOKEN")
UBIDOTS_DEVICE_LABEL = os.getenv("UBIDOTS_DEVICE_LABEL", "aira-chia-nodo1")
UBIDOTS_TOPIC = f"/v1.6/devices/{UBIDOTS_DEVICE_LABEL}"

cloud_mqtt_client = None
cloud_mqtt_connected = False

SUPABASE_TABLE = "readings"
AI_RECOMMENDATION_TABLE = "ai_recommendations"
AI_REQUESTS_TABLE = "ai_requests"

GEMINI_MODEL = "gemini-2.5-flash"

# AIRA no debe consumir tokens todo el tiempo.
# Automática: solo en Peligro, máximo cada 10 minutos.
# Manual: solo si el usuario la solicita, máximo cada 60 segundos.
AI_DANGER_COOLDOWN_SECONDS = 600
AI_MANUAL_COOLDOWN_SECONDS = 60

last_danger_ai_ts = None
last_manual_ai_ts = None
last_danger_state = None


# =========================
# Variables de entorno
# =========================



SUPABASE_URL = os.getenv("SUPABASE_URL")
SUPABASE_SERVICE_ROLE_KEY = os.getenv("SUPABASE_SERVICE_ROLE_KEY")
GEMINI_API_KEY = os.getenv("GEMINI_API_KEY")


# =========================
# Clientes externos
# =========================

supabase: Client | None = None

if SUPABASE_URL and SUPABASE_SERVICE_ROLE_KEY:
    supabase = create_client(SUPABASE_URL, SUPABASE_SERVICE_ROLE_KEY)
else:
    print("Advertencia: Supabase no configurado. Solo se guardara en SQLite.")


gemini_client = None

if GEMINI_API_KEY:
    gemini_client = genai.Client(api_key=GEMINI_API_KEY)
else:
    print("Advertencia: Gemini no configurado. AIRA no generara recomendaciones.")


# =========================
# Base de datos local SQLite
# =========================

def init_db():
    conn = sqlite3.connect(DB_PATH, timeout=10)
    cur = conn.cursor()

    cur.execute("PRAGMA journal_mode=WAL;")
    cur.execute("PRAGMA busy_timeout=5000;")

    cur.execute("""
        CREATE TABLE IF NOT EXISTS readings (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp_utc TEXT NOT NULL,
            pm1 REAL,
            pm25 REAL,
            pm10 REAL,
            nh3 REAL,
            temperature REAL,
            humidity REAL,
            pressure REAL,
            estado TEXT,
            causa TEXT,
            muted INTEGER,
            raw_json TEXT NOT NULL
        )
    """)

    conn.commit()
    conn.close()


def save_reading_sqlite(row):
    conn = sqlite3.connect(DB_PATH, timeout=10)
    cur = conn.cursor()

    cur.execute("PRAGMA busy_timeout=5000;")

    cur.execute("""
        INSERT INTO readings (
            timestamp_utc,
            pm1,
            pm25,
            pm10,
            nh3,
            temperature,
            humidity,
            pressure,
            estado,
            causa,
            muted,
            raw_json
        )
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    """, (
        row["timestamp_utc"],
        row["pm1"],
        row["pm25"],
        row["pm10"],
        row["nh3"],
        row["temperature"],
        row["humidity"],
        row["pressure"],
        row["estado"],
        row["causa"],
        1 if row["muted"] else 0,
        json.dumps(row["raw_json"], ensure_ascii=False)
    ))

    conn.commit()
    conn.close()

# =========================
# Apagar alarmas remotamente desde el dashboard
# =========================

def fetch_pending_alarm_command():
    if supabase is None:
        return None

    try:
        response = (
            supabase
            .table(ALARM_COMMANDS_TABLE)
            .select("*")
            .eq("status", "pending")
            .order("created_at", desc=False)
            .limit(1)
            .execute()
        )

        if response.data:
            return response.data[0]

        return None

    except Exception as e:
        print(f"Error consultando comandos de alarma: {e}")
        return None


def mark_alarm_command_processed(command_id, ok=True, error=None):
    if supabase is None or command_id is None:
        return

    try:
        payload = {
            "status": "processed" if ok else "error",
            "processed_at": datetime.now(timezone.utc).isoformat(),
            "error": error
        }

        (
            supabase
            .table(ALARM_COMMANDS_TABLE)
            .update(payload)
            .eq("id", command_id)
            .execute()
        )

    except Exception as e:
        print(f"Error actualizando comando de alarma: {e}")


def publish_alarm_command_to_esp(client, command):
    try:
        payload = {
            "command": "set_alarm_mute",
            "muted": bool(command.get("muted"))
        }

        payload_json = json.dumps(payload, separators=(",", ":"))
        
        result = client.publish(MQTT_CMD_TOPIC, payload_json, qos=0)

        if result.rc == mqtt.MQTT_ERR_SUCCESS:
            print(f"Comando enviado a ESP32 | {payload_json}")
            return True

        print(f"Error publicando comando a ESP32. rc={result.rc}")
        return False

    except Exception as e:
        print(f"Error enviando comando de alarma a ESP32: {e}")
        return False


def process_alarm_command_logic(client):
    pending_command = fetch_pending_alarm_command()

    if not pending_command:
        return False

    ok = publish_alarm_command_to_esp(client, pending_command)

    if ok:
        mark_alarm_command_processed(pending_command.get("id"), ok=True)
    else:
        mark_alarm_command_processed(
            pending_command.get("id"),
            ok=False,
            error="No se pudo publicar el comando MQTT hacia la ESP32"
        )

    return ok

# =========================
# Normalización de datos
# =========================

def normalize_payload(payload):
    timestamp_utc = datetime.now(timezone.utc).isoformat()

    return {
        "timestamp_utc": timestamp_utc,
        "pm1": payload.get("pm1"),
        "pm25": payload.get("pm25"),
        "pm10": payload.get("pm10"),
        "nh3": payload.get("nh3"),
        "temperature": payload.get("t"),
        "humidity": payload.get("h"),
        "pressure": payload.get("p"),
        "estado": payload.get("estado"),
        "causa": payload.get("causa"),
        "muted": bool(payload.get("muted")),
        "raw_json": payload
    }


# =========================
# Supabase: lecturas
# =========================

def save_reading_supabase(row):
    if supabase is None:
        return False

    try:
        supabase.table(SUPABASE_TABLE).insert(row).execute()
        return True

    except Exception as e:
        print(f"Error subiendo lectura a Supabase: {e}")
        return False


# =========================
# AIRA: prompt contextualizado
# =========================

def build_ai_prompt(row, trigger_source):
    return f"""
Eres AIRA, el Asistente Inteligente de Recomendación Ambiental del sistema IoT de monitoreo de calidad del aire para autoridades locales de Chía, Cundinamarca, en la región Sabana Centro.

AIRA no reemplaza el criterio de las autoridades ni emite diagnósticos médicos. Su función es transformar lecturas ambientales en recomendaciones operativas, preventivas y proporcionales al nivel de riesgo detectado.

Contexto del despliegue:
El sistema IoT está ubicado en una zona periurbana de Chía, Cundinamarca, con influencia de vías locales, áreas residenciales y actividad agropecuaria cercana. En este entorno pueden presentarse incrementos temporales de material particulado por tránsito, polvo suspendido, combustión o actividades productivas, así como variaciones en gases ambientales asociados al contexto agropecuario y urbano-periurbano.

Objetivo:
Apoyar a las autoridades locales en la toma de decisiones preventivas frente a posibles episodios de deterioro de calidad del aire, usando datos capturados por sensores IoT y procesados por el sistema.

Lectura ambiental actual:
- PM1.0: {row.get("pm1")} ug/m3
- PM2.5: {row.get("pm25")} ug/m3
- PM10: {row.get("pm10")} ug/m3
- NH3 estimado: {row.get("nh3")} ppm
- Temperatura: {row.get("temperature")} °C
- Humedad relativa: {row.get("humidity")} %
- Presión atmosférica: {row.get("pressure")} hPa
- Estado del sistema: {row.get("estado")}
- Causa principal reportada por el sistema: {row.get("causa")}
- Tipo de activación de AIRA: {trigger_source}
- Timestamp UTC: {row.get("timestamp_utc")}

Reglas de respuesta:
1. Responde siempre en español.
2. Máximo 90 palabras.
3. Empieza exactamente con: "AIRA recomienda:"
4. No inventes fuentes contaminantes específicas.
5. No afirmes que existe una emergencia si los datos no lo justifican.
6. No des diagnóstico médico ni recomendaciones clínicas.
7. No menciones que eres un modelo de IA.
8. Entrega acciones operativas para autoridades locales.
9. Si el estado es "Peligro", recomienda activar protocolo preventivo, reducir exposición cercana, verificar posibles fuentes de contaminación y continuar monitoreo.
10. Si el estado es "Precaución", recomienda monitoreo reforzado, revisión del entorno cercano y medidas preventivas proporcionales.
11. Si el estado es "Normal" y la solicitud fue manual, recomienda mantener monitoreo y revisar estabilidad del sistema.
12. Considera que la zona es periurbana, con posible influencia vial, residencial y agropecuaria.
13. Mantén un tono técnico, claro y accionable.

Formato de salida:
AIRA recomienda: ...
"""


def generate_ai_recommendation(row, trigger_source):
    if gemini_client is None:
        return None

    try:
        prompt = build_ai_prompt(row, trigger_source)

        response = gemini_client.models.generate_content(
            model=GEMINI_MODEL,
            contents=prompt
        )

        return response.text.strip()

    except Exception as e:
        print(f"Error generando recomendacion AIRA: {e}")
        return None


def save_ai_recommendation_supabase(row, recommendation, trigger_source):
    if supabase is None or not recommendation:
        return False

    try:
        payload = {
            "timestamp_utc": row.get("timestamp_utc"),
            "trigger_source": trigger_source,
            "estado": row.get("estado"),
            "causa": row.get("causa"),
            "recommendation": recommendation,
            "model": GEMINI_MODEL,
            "input_json": row
        }

        supabase.table(AI_RECOMMENDATION_TABLE).insert(payload).execute()
        return True

    except Exception as e:
        print(f"Error subiendo recomendacion AIRA a Supabase: {e}")
        return False


# =========================
# AIRA automática: solo Peligro
# =========================

def should_generate_danger_ai(row):
    global last_danger_ai_ts, last_danger_state

    estado = str(row.get("estado") or "").lower()

    if "peligro" not in estado:
        last_danger_state = row.get("estado")
        return False

    if gemini_client is None:
        return False

    now = datetime.now(timezone.utc)

    if last_danger_ai_ts is None:
        return True

    elapsed = (now - last_danger_ai_ts).total_seconds()

    if last_danger_state != row.get("estado"):
        return True

    return elapsed >= AI_DANGER_COOLDOWN_SECONDS


# =========================
# AIRA manual: solicitud desde dashboard
# =========================

def fetch_pending_ai_request():
    if supabase is None:
        return None

    try:
        response = (
            supabase
            .table(AI_REQUESTS_TABLE)
            .select("*")
            .eq("status", "pending")
            .order("created_at", desc=False)
            .limit(1)
            .execute()
        )

        if response.data:
            return response.data[0]

        return None

    except Exception as e:
        print(f"Error consultando solicitudes AIRA: {e}")
        return None


def mark_ai_request_processed(request_id, ok=True, error=None):
    if supabase is None or request_id is None:
        return

    try:
        payload = {
            "status": "processed" if ok else "error",
            "processed_at": datetime.now(timezone.utc).isoformat(),
            "error": error
        }

        (
            supabase
            .table(AI_REQUESTS_TABLE)
            .update(payload)
            .eq("id", request_id)
            .execute()
        )

    except Exception as e:
        print(f"Error actualizando solicitud AIRA: {e}")


def should_process_manual_ai():
    global last_manual_ai_ts

    if gemini_client is None:
        return False

    now = datetime.now(timezone.utc)

    if last_manual_ai_ts is None:
        return True

    elapsed = (now - last_manual_ai_ts).total_seconds()
    return elapsed >= AI_MANUAL_COOLDOWN_SECONDS


def process_ai_logic(row):
    global last_danger_ai_ts, last_danger_state, last_manual_ai_ts

    ai_ok = False

    # AIRA automática: solo si llega estado Peligro
    if should_generate_danger_ai(row):
        recommendation = generate_ai_recommendation(row, "automatic_peligro")
        ai_ok = save_ai_recommendation_supabase(row, recommendation, "automatic_peligro")

        if recommendation and ai_ok:
            last_danger_ai_ts = datetime.now(timezone.utc)
            last_danger_state = row.get("estado")
            print(f"AIRA automatic_peligro | {recommendation}")

    # AIRA manual: solo si hay solicitud pendiente desde el dashboard
    pending_request = fetch_pending_ai_request()

    if pending_request and should_process_manual_ai():
        recommendation = generate_ai_recommendation(row, "manual_dashboard")
        manual_ok = save_ai_recommendation_supabase(row, recommendation, "manual_dashboard")

        if recommendation and manual_ok:
            last_manual_ai_ts = datetime.now(timezone.utc)
            mark_ai_request_processed(pending_request.get("id"), ok=True)
            print(f"AIRA manual_dashboard | {recommendation}")
        else:
            mark_ai_request_processed(
                pending_request.get("id"),
                ok=False,
                error="No se pudo generar o guardar la recomendacion de AIRA"
            )

    return ai_ok


# =========================
# MQTT callbacks
# =========================

def estado_to_code(estado):
    clean = str(estado or "").lower()

    if "peligro" in clean:
        return 2
    if "precauc" in clean:
        return 1
    return 0


def on_cloud_mqtt_connect(client, userdata, flags, rc, *extra):
    global cloud_mqtt_connected

    if rc == 0:
        cloud_mqtt_connected = True
        print("Conectado a MQTT cloud Ubidots")
    else:
        cloud_mqtt_connected = False
        print(f"Error conectando a MQTT cloud Ubidots. Codigo: {rc}")


def on_cloud_mqtt_disconnect(client, userdata, rc, *extra):
    global cloud_mqtt_connected

    cloud_mqtt_connected = False
    print(f"MQTT cloud Ubidots desconectado. Codigo: {rc}")


def init_cloud_mqtt():
    global cloud_mqtt_client

    if not UBIDOTS_MQTT_ENABLED:
        print("MQTT cloud Ubidots desactivado.")
        return

    if not UBIDOTS_TOKEN:
        print("Advertencia: UBIDOTS_TOKEN no configurado. No se publicara por MQTT cloud.")
        return

    cloud_mqtt_client = mqtt.Client(client_id="raspberry-gateway-aira")
    cloud_mqtt_client.username_pw_set(UBIDOTS_TOKEN, "")

    cloud_mqtt_client.on_connect = on_cloud_mqtt_connect
    cloud_mqtt_client.on_disconnect = on_cloud_mqtt_disconnect

    try:
        cloud_mqtt_client.connect(
            UBIDOTS_MQTT_HOST,
            UBIDOTS_MQTT_PORT,
            keepalive=60
        )
        cloud_mqtt_client.loop_start()

        print(f"MQTT cloud configurado: {UBIDOTS_MQTT_HOST}:{UBIDOTS_MQTT_PORT}")
        print(f"Topic Ubidots: {UBIDOTS_TOPIC}")

    except Exception as e:
        print(f"Error inicializando MQTT cloud Ubidots: {e}")


def build_ubidots_payload(row):
    return {
        "pm1": row.get("pm1"),
        "pm25": row.get("pm25"),
        "pm10": row.get("pm10"),
        "nh3": row.get("nh3"),
        "temperature": row.get("temperature"),
        "humidity": row.get("humidity"),
        "pressure": row.get("pressure"),
        "estado_code": estado_to_code(row.get("estado")),
        "muted": 1 if row.get("muted") else 0
    }


def publish_cloud_mqtt(row):
    if not UBIDOTS_MQTT_ENABLED:
        return False

    if cloud_mqtt_client is None:
        return False

    try:
        payload = build_ubidots_payload(row)
        payload_json = json.dumps(payload)

        result = cloud_mqtt_client.publish(
            UBIDOTS_TOPIC,
            payload_json,
            qos=0
        )

        if result.rc == mqtt.MQTT_ERR_SUCCESS:
            return True

        print(f"Error publicando MQTT cloud. rc={result.rc}")
        return False

    except Exception as e:
        print(f"Error publicando a MQTT cloud Ubidots: {e}")
        return False
    


def on_connect(client, userdata, flags, rc, *extra):
    if rc == 0:
        print("Conectado a MQTT")
        client.subscribe(MQTT_TOPIC)
        print(f"Escuchando topic: {MQTT_TOPIC}")
    else:
        print(f"Error conectando a MQTT. Codigo: {rc}")


def on_message(client, userdata, msg):
    try:
        text = msg.payload.decode("utf-8")
        payload = json.loads(text)

        row = normalize_payload(payload)

        save_reading_sqlite(row)
        cloud_ok = save_reading_supabase(row)
        ai_ok = process_ai_logic(row)

        mqtt_cloud_ok = publish_cloud_mqtt(row)

        alarm_cmd_ok = process_alarm_command_logic(client)

        print(
            f"Guardado | "
            f"PM2.5={row['pm25']} | "
            f"Estado={row['estado']} | "
            f"Causa={row['causa']} | "
            f"SQLite=OK | "
            f"Supabase={'OK' if cloud_ok else 'NO'} | "
            f"AIRA={'OK' if ai_ok else 'NO'} | "
            f"AlarmCmd={'OK' if alarm_cmd_ok else 'NO'} | "
            f"MQTT_Cloud={'OK' if mqtt_cloud_ok else 'NO'}"
        )
    
    except json.JSONDecodeError:
        print(f"Mensaje recibido no es JSON valido: {msg.payload.decode('utf-8', errors='ignore')}")

    except Exception as e:
        print(f"Error procesando mensaje: {e}")


# =========================
# Main
# =========================

def main():
    init_db()
    init_cloud_mqtt()
    client = mqtt.Client()
    client.on_connect = on_connect
    client.on_message = on_message

    print("Iniciando gateway local + nube + AIRA...")
    print(f"Base de datos local: {DB_PATH}")
    print(f"Broker MQTT: {MQTT_HOST}:{MQTT_PORT}")
    print(f"Topic: {MQTT_TOPIC}")
    print(f"Supabase configurado: {'SI' if supabase else 'NO'}")
    print(f"AIRA configurada: {'SI' if gemini_client else 'NO'}")
    print(f"MQTT cloud Ubidots: {'SI' if UBIDOTS_MQTT_ENABLED else 'NO'}")

    client.connect(MQTT_HOST, MQTT_PORT, keepalive=60)
    client.loop_forever()


if __name__ == "__main__":
    main()