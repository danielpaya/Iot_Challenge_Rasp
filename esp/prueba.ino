#include <HardwareSerial.h>
#include <Wire.h>
#include <math.h>
#include <WiFi.h>
#include <WebServer.h>
#include <PubSubClient.h>

// =====================================================
// DEFINICIÓN DE TIPOS
// =====================================================
enum Level { NORMAL=0, PRECAUCION=1, PELIGRO=2 };

// =====================================================
// CREDENCIALES WIFI Y DASHBOARD
// =====================================================
const char* ssid = "vivoY36";
const char* password = "DanielP17";
const char* www_pass = "1234";

WebServer server(80);

WiFiClient espClient;
PubSubClient mqttClient(espClient);

const char* mqtt_server = "10.238.66.137";
const int mqtt_port = 1883;
const char* mqtt_topic = "sabana/aire/nodo1";

unsigned long lastMqttPublishMs = 0;
const unsigned long MQTT_INTERVAL_MS = 2000;

const char* headerkeys[] = {"Cookie"};
size_t headerkeyssize = sizeof(headerkeys) / sizeof(char*);

// =====================================================
// FREERTOS: MULTITHREADING Y PROTECCIÓN DE MEMORIA
// =====================================================
TaskHandle_t TaskHardware;
SemaphoreHandle_t dataMutex;

// =====================================================
// ESTADO GLOBAL PARA EL SERVIDOR WEB (Protegidas por Mutex)
// =====================================================
bool buzzerMuted = false;
String currentEstado = "Normal";
String currentCausa = "Ninguna";
Level currentLevel = NORMAL;

uint16_t pm1_0 = 0, pm2_5 = 0, pm10  = 0;
float nh3_ppm = 0.0f, tempC = NAN, pres_hPa = NAN, humRH = NAN;

// =====================================================
//  SENSORES Y ACTUADORES
// =====================================================
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
Adafruit_BME280 bme;

#include <LiquidCrystal_I2C.h>
LiquidCrystal_I2C lcd(0x27, 16, 2);

const int LED_R = 25;
const int LED_G = 26;
const int LED_B = 27;
const int RGB_COMMON_ANODE = 0;

const int BUZZER_PIN = 33;    
const int BUZZER_ACTIVE_HIGH = 1; 

// =====================================================
// CONFIGURACIÓN DEL MODELO Y SENSORES
// =====================================================
const int N_FEAT = 4;
const int K = 3;
const float CENTROIDS[3][4] = {
  {0.067695f, 0.066557f, 0.073262f, 0.766916f},
  {0.477728f, 0.471661f, 0.430091f, 0.752588f},
  {0.124728f, 0.115632f, 0.105316f, 0.248652f},
};
const float P75[4] = {0.407268f, 0.376813f, 0.326889f, 0.826943f};
const float P90[4] = {0.528457f, 0.544942f, 0.512827f, 0.911577f};
const int CLUSTER_LEVEL[3] = {1, 2, 0};

HardwareSerial dustSerial(1);
const int DUST_RX = 16;
const int DUST_TX = 17;
const int DUST_BAUD = 9600;
#define LENG 31
uint8_t bufSEN[LENG];

const int MQ135_PIN = 34;
const float ADC_VREF = 3.3f;
const int ADC_MAX = 4095;
const float RL = 10000.0f;
const float Ro = 140000.0f;
const float MQ_A = 60.0f;
const float MQ_B = -1.2f;

const int WIN = 6;
float histPM25[WIN];
float histNH3[WIN];
int histCount = 0;
int histIdx = 0;

int screenIndex = 0;

// =====================================================
// PÁGINAS WEB (PROGMEM)
// =====================================================
const char login_html[] PROGMEM = R"=====(
<!DOCTYPE html>
<html lang="es">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Acceso Dashboard</title>
    <style>
        body { font-family: 'Segoe UI', Tahoma, sans-serif; background-color: #f4f7f6; display: flex; justify-content: center; align-items: center; height: 100vh; margin: 0; }
        .login-box { background: white; padding: 40px; border-radius: 10px; box-shadow: 0 4px 15px rgba(0,0,0,0.1); text-align: center; max-width: 300px; width: 100%; }
        h2 { margin-top: 0; color: #333; }
        input[type="password"] { width: 100%; padding: 12px; margin: 15px 0; border: 1px solid #ddd; border-radius: 5px; box-sizing: border-box; font-size: 1rem; }
        button { width: 100%; padding: 12px; background-color: #007bff; color: white; border: none; border-radius: 5px; font-size: 1rem; cursor: pointer; transition: 0.3s; font-weight: bold;}
        button:hover { background-color: #0056b3; }
        .error { color: red; font-size: 0.9rem; margin-top: 10px; display: none; }
    </style>
</head>
<body>
    <div class="login-box">
        <h2>Dashboard Ambiental</h2>
        <form action="/login" method="POST">
            <input type="password" name="pass" placeholder="Ingresa la contraseña" required>
            <button type="submit">Entrar</button>
            <p class="error" id="errMsg">Contraseña incorrecta</p>
        </form>
    </div>
    <script>
        if(window.location.search.includes('err=1')) document.getElementById('errMsg').style.display = 'block';
    </script>
</body>
</html>
)=====";

const char index_html[] PROGMEM = R"=====(
<!DOCTYPE html>
<html lang="es">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Panel de Control - Calidad del Aire</title>
    <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
    <style>
        :root { --bg: #f4f7f6; --card: #ffffff; --text: #333; --accent: #007bff; }
        body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background-color: var(--bg); color: var(--text); margin: 0; padding: 20px; }
        .header { display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; margin-bottom: 20px; gap: 10px;}
        h1 { margin: 0; font-size: 1.5rem; }
        .controls { display: flex; gap: 10px; }
        .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(140px, 1fr)); gap: 15px; margin-bottom: 20px; }
        .card { background: var(--card); padding: 15px; border-radius: 10px; box-shadow: 0 4px 6px rgba(0,0,0,0.05); text-align: center; }
        .card h3 { margin: 0 0 10px 0; font-size: 0.9rem; color: #666; }
        .card p { margin: 0; font-size: 1.3rem; font-weight: bold; }
        .alert-banner { display: block; padding: 15px; border-radius: 10px; margin-bottom: 20px; font-weight: bold; text-align: center; color: white; background-color: #4CAF50; transition: 0.3s;}
        .alert-precaucion { background-color: #ff9800; }
        .alert-peligro { background-color: #f44336; }
        button { border: none; padding: 10px 20px; border-radius: 5px; cursor: pointer; font-size: 1rem; font-weight: bold; transition: 0.3s; color: white; }
        .btn-active { background-color: #f44336; }
        .btn-muted { background-color: #ff9800; }
        .btn-disabled { background-color: #e0e0e0; color: #888; cursor: not-allowed; }
        .btn-logout { background-color: #607D8B; }
        .btn-logout:hover { background-color: #455A64; }
        .charts-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(350px, 1fr)); gap: 15px; }
        .chart-container { background: var(--card); padding: 15px; border-radius: 10px; box-shadow: 0 4px 6px rgba(0,0,0,0.05); height: 250px; }
        .chart-pm-full { grid-column: 1 / -1; height: 300px; }
    </style>
</head>
<body>
    <div class="header">
        <h1>Dashboard Ambiental</h1>
        <div class="controls">
            <button id="muteBtn" onclick="toggleMute()" class="btn-disabled" disabled>Cargando...</button>
            <button onclick="window.location.href='/logout'" class="btn-logout">Salir</button>
        </div>
    </div>
    <div id="alertBanner" class="alert-banner">
        Estado Actual: <span id="estadoText">Normal</span> | Evento Detectado: <span id="causaText">Ninguna</span>
    </div>
    <div class="grid">
        <div class="card"><h3>PM 1.0</h3><p><span id="v_pm1">0</span> µg/m³</p></div>
        <div class="card"><h3>PM 2.5</h3><p><span id="v_pm25">0</span> µg/m³</p></div>
        <div class="card"><h3>PM 10</h3><p><span id="v_pm10">0</span> µg/m³</p></div>
        <div class="card"><h3>NH3 (Est)</h3><p><span id="v_nh3">0.0</span> ppm</p></div>
        <div class="card"><h3>Temperatura</h3><p><span id="v_t">0.0</span> °C</p></div>
        <div class="card"><h3>Humedad</h3><p><span id="v_h">0.0</span> %</p></div>
        <div class="card"><h3>Presión</h3><p><span id="v_p">0.0</span> hPa</p></div>
    </div>
    <div class="charts-grid">
        <div class="chart-container chart-pm-full"><canvas id="pmChart"></canvas></div>
        <div class="chart-container"><canvas id="nh3Chart"></canvas></div>
        <div class="chart-container"><canvas id="tempChart"></canvas></div>
        <div class="chart-container"><canvas id="humChart"></canvas></div>
        <div class="chart-container"><canvas id="presChart"></canvas></div>
    </div>

    <script>
        const chartConfig = (label, color, unit) => ({
            type: 'line',
            data: { labels: [], datasets: [{ label: label, borderColor: color, backgroundColor: color + '33', data: [], tension: 0.4, fill: true }] },
            options: { responsive: true, maintainAspectRatio: false, scales: { y: { title: { display: true, text: unit } } } }
        });

        const pmChart = new Chart(document.getElementById('pmChart').getContext('2d'), {
            type: 'line',
            data: { labels: [], datasets: [
                { label: 'PM 1.0', borderColor: '#4CAF50', data: [], tension: 0.4 },
                { label: 'PM 2.5', borderColor: '#2196F3', data: [], tension: 0.4 },
                { label: 'PM 10', borderColor: '#9C27B0', data: [], tension: 0.4 }
            ]},
            options: { responsive: true, maintainAspectRatio: false, scales: { y: { title: { display: true, text: 'µg/m³' } } } }
        });

        const nh3Chart = new Chart(document.getElementById('nh3Chart').getContext('2d'), chartConfig('NH3 (Gas)', '#FF9800', 'ppm'));
        const tempChart = new Chart(document.getElementById('tempChart').getContext('2d'), chartConfig('Temperatura', '#F44336', '°C'));
        const humChart = new Chart(document.getElementById('humChart').getContext('2d'), chartConfig('Humedad', '#00BCD4', '%'));
        const presChart = new Chart(document.getElementById('presChart').getContext('2d'), chartConfig('Presión', '#607D8B', 'hPa'));

        const fetchData = () => {
            fetch('/api/data').then(response => {
                if(response.status === 401) { window.location.reload(); throw new Error('Sesión expirada'); }
                return response.json();
            }).then(data => {
                document.getElementById('v_pm1').innerText = data.pm1;
                document.getElementById('v_pm25').innerText = data.pm25;
                document.getElementById('v_pm10').innerText = data.pm10;
                document.getElementById('v_nh3').innerText = data.nh3;
                document.getElementById('v_t').innerText = data.t !== null ? data.t : '--';
                document.getElementById('v_h').innerText = data.h !== null ? data.h : '--';
                document.getElementById('v_p').innerText = data.p !== null ? data.p : '--';

                const banner = document.getElementById('alertBanner');
                document.getElementById('estadoText').innerText = data.estado;
                document.getElementById('causaText').innerText = data.estado === 'Normal' ? 'Ninguna' : data.causa;
                
                banner.className = 'alert-banner'; 
                if(data.estado === 'Precaucion') banner.classList.add('alert-precaucion');
                else if(data.estado === 'Peligro') banner.classList.add('alert-peligro');

                const btn = document.getElementById('muteBtn');
                if (data.estado === 'Normal') {
                    btn.disabled = true; btn.className = 'btn-disabled'; btn.innerText = "Sin Alertas";
                } else {
                    btn.disabled = false;
                    if (data.muted) { btn.className = 'btn-muted'; btn.innerText = "Alarma: SILENCIADA"; } 
                    else { btn.className = 'btn-active'; btn.innerText = "Alarma: SONANDO (Silenciar)"; }
                }

                const now = new Date();
                const timeStr = now.getHours() + ':' + now.getMinutes().toString().padStart(2, '0') + ':' + now.getSeconds().toString().padStart(2, '0');
                
                const updateChart = (chart, valueArray) => {
                    if(chart.data.labels.length > 30) { chart.data.labels.shift(); chart.data.datasets.forEach(d => d.data.shift()); }
                    chart.data.labels.push(timeStr);
                    valueArray.forEach((val, i) => chart.data.datasets[i].data.push(val));
                    chart.update();
                };

                updateChart(pmChart, [data.pm1, data.pm25, data.pm10]);
                updateChart(nh3Chart, [data.nh3]);
                updateChart(tempChart, [data.t]);
                updateChart(humChart, [data.h]);
                updateChart(presChart, [data.p]);
            }).catch(e => console.log(e));
        };

        const toggleMute = () => { fetch('/api/mute', { method: 'POST' }).then(() => fetchData()); };
        setInterval(fetchData, 2000);
        fetchData(); 
    </script>
</body>
</html>
)=====";


// =====================================================
// FUNCIONES AUXILIARES 
// =====================================================
float clamp01(float x){ return (x < 0.0f) ? 0.0f : ((x > 1.0f) ? 1.0f : x); }
float sqf(float x){ return x*x; }

float normPM1(float ugm3){  return clamp01(ugm3 / 150.0f); }
float normPM25(float ugm3){ return clamp01(ugm3 / 150.0f); }
float normPM10(float ugm3){ return clamp01(ugm3 / 200.0f); }
float normNH3(float ppm){   return clamp01(ppm  / 50.0f);  }

const char* levelName(Level L){
  if(L==NORMAL) return "Normal";
  if(L==PRECAUCION) return "Precaucion";
  return "Peligro";
}

void pwmWriteRgb(int r, int g, int b){
  if(RGB_COMMON_ANODE){ r = 255 - r; g = 255 - g; b = 255 - b; }
  ledcWrite(LED_R, r); ledcWrite(LED_G, g); ledcWrite(LED_B, b);
}

void setLedByLevel(Level L){
  if(L==NORMAL)          pwmWriteRgb(0, 255, 0);
  else if(L==PRECAUCION) pwmWriteRgb(255, 120, 0);
  else                   pwmWriteRgb(255, 0, 0);
}

void buzzerWrite(bool on){
  if(BUZZER_ACTIVE_HIGH) digitalWrite(BUZZER_PIN, on ? HIGH : LOW);
  else digitalWrite(BUZZER_PIN, on ? LOW : HIGH);
}

void playWifiConnectedBeep() {
  // Dos pitidos cortos para indicar conexion exitosa
  buzzerWrite(true);
  delay(200);
  buzzerWrite(false);
  delay(80);
  buzzerWrite(true);
  delay(200);
  buzzerWrite(false);
}

// =====================================================
// MODELO DE DATOS Y FUSIÓN SENSORIAL
// =====================================================
bool checkValueSEN(uint8_t *thebuf, uint8_t leng) {
  uint16_t receiveSum = 0;
  for (int i = 0; i < (leng - 2); i++) receiveSum += thebuf[i];
  receiveSum += 0x42;
  return (receiveSum == (((uint16_t)thebuf[leng - 2] << 8) + thebuf[leng - 1]));
}

bool readSEN0177(uint16_t &pm1, uint16_t &pm25, uint16_t &pm10_) {
  if (dustSerial.find(0x42)) {
    delay(50);
    size_t n = dustSerial.readBytes(bufSEN, LENG);
    if (n != LENG || bufSEN[0] != 0x4D || !checkValueSEN(bufSEN, LENG)) return false;
    pm1  = ((uint16_t)bufSEN[3] << 8) + bufSEN[4];
    pm25 = ((uint16_t)bufSEN[5] << 8) + bufSEN[6];
    pm10_= ((uint16_t)bufSEN[7] << 8) + bufSEN[8];
    return true;
  }
  return false;
}

float readNH3ppm(){
  int adcValue = analogRead(MQ135_PIN);
  float voltage = (adcValue / (float)ADC_MAX) * ADC_VREF;
  if (voltage < 0.01f) voltage = 0.01f;
  float rs = RL * ((ADC_VREF - voltage) / voltage);
  float ratio = rs / Ro;
  float ppm = MQ_A * pow(ratio, MQ_B);
  return (ppm < 0) ? 0 : ppm;
}

Level voteKMeans(const float x[4], int &bestCluster, float &bestDist){
  bestDist = 1e9f;
  for(int k=0;k<K;k++){
    float d = 0.0f;
    for(int j=0;j<4;j++) d += sqf(CENTROIDS[k][j] - x[j]);
    if(d < bestDist){ bestDist = d; bestCluster = k; }
  }
  return (Level)CLUSTER_LEVEL[bestCluster];
}

// NUEVO: Fusión de Datos Sensoriales
Level voteFlagsFusion(const float x[4], float t, float h, float p){
  int danger = 0;
  int caution = 0;

  for(int j=0; j<4; j++){
    if(x[j] > P90[j]) danger++;
    else if(x[j] > P75[j]) caution++;
  }

  int envStress = 0;
  
  if(!isnan(t)) {
    if(t > 30.0f || t < 5.0f) envStress += 1; 
  }
  if(!isnan(h)) {
    if(h > 75.0f) envStress += 1; 
  }
  if(!isnan(p)) {
    // Calibrado para la altitud de la Sabana de Bogotá (Margen de anomalía respecto a ~740 hPa)
    if(p < 710.0f || p > 770.0f) envStress += 1; 
  }

  if(danger >= 2) return PELIGRO;
  if(danger == 1 && envStress >= 1) return PELIGRO; 
  if((danger + caution) >= 2) return PRECAUCION;
  if((danger + caution) == 1 && envStress >= 2) return PRECAUCION;

  return NORMAL;
}

Level voteTrend(){
  if(histCount < WIN) return NORMAL;
  int i_now  = (histIdx - 1 + WIN) % WIN; 
  int i_prev = (histIdx - 4 + WIN) % WIN; 
  float dp = histPM25[i_now] - histPM25[i_prev]; 
  float dn = histNH3[i_now]  - histNH3[i_prev];  
  if((dp > 0.0f && histPM25[i_now] > P75[1]) || (dn > 0.0f && histNH3[i_now] > P75[3])){
    if(histPM25[i_now] > P90[1] || histNH3[i_now] > P90[3]) return PELIGRO;
    return PRECAUCION;
  }
  return NORMAL;
}

Level majority(Level a, Level b, Level c){
  int counts[3] = {0,0,0};
  counts[(int)a]++; counts[(int)b]++; counts[(int)c]++;
  int best = 0;
  if(counts[1] > counts[best]) best = 1;
  if(counts[2] > counts[best]) best = 2;
  return (Level)best;
}

const char* causeLabel(const float x[4]){
  bool pm_high  = (x[1] > P90[1]) || (x[2] > P90[2]); 
  bool nh3_high = (x[3] > P90[3]);                    
  if(pm_high && nh3_high) return "Mixto";
  if(pm_high)             return "PM";
  if(nh3_high)            return "NH3";
  return "Bajo";
}

void lcdShow(Level fin, const char* causa, uint16_t p1, uint16_t p25, uint16_t p10, float nh3, float t, float h){
  lcd.clear();
  if(screenIndex == 0){
    lcd.setCursor(0,0); lcd.print("E:"); lcd.print(levelName(fin));
    lcd.setCursor(0,1); lcd.print("C:"); lcd.print(causa);
  }
  else if(screenIndex == 1){
    lcd.setCursor(0,0); lcd.print("PM1: "); lcd.print(p1);
    lcd.setCursor(0,1); lcd.print("PM25:"); lcd.print(p25);
  }
  else if(screenIndex == 2){
    lcd.setCursor(0,0); lcd.print("PM10:"); lcd.print(p10);
    lcd.setCursor(0,1); lcd.print("NH3: "); lcd.print(nh3,1);
  }
  else if(screenIndex == 3){
    lcd.setCursor(0,0); lcd.print("T: "); 
    if(!isnan(t)) lcd.print(t, 1); else lcd.print("--");
    lcd.setCursor(0,1); lcd.print("H: ");
    if(!isnan(h) && h > 0.1f) lcd.print(h, 1); else lcd.print("--");
  }
}

// =====================================================
// HILO INDEPENDIENTE (FreeRTOS) - HARDWARE Y SENSORES
// =====================================================
void hardwareLogicTask(void * pvParameters) {
  unsigned long lastSensorReadMs = 0;
  unsigned long lastScreenMs = 0;

  for(;;) {
    unsigned long now = millis();

    // 1) Actualización constante del Buzzer (No bloqueante)
    bool isMuted_local;
    Level lvl_local;
    
    // Leemos variables de forma segura
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    isMuted_local = buzzerMuted;
    lvl_local = currentLevel;
    xSemaphoreGive(dataMutex);

    if(isMuted_local || lvl_local == NORMAL){
      buzzerWrite(false);
    } else {
      if(lvl_local == PRECAUCION){
        buzzerWrite((now % 2000) < 150);
      } else {
        buzzerWrite((now % 400) < 200);
      }
    }

    // 2) Lectura de Sensores y Lógica (Cada 500ms)
    if(now - lastSensorReadMs > 500) {
      lastSensorReadMs = now;

      float l_nh3 = readNH3ppm();
      uint16_t l_pm1 = 0, l_pm25 = 0, l_pm10 = 0;
      bool senOk = readSEN0177(l_pm1, l_pm25, l_pm10);
      
      float l_t = bme.readTemperature();
      float l_p = NAN, l_h = NAN;
      if(!isnan(l_t)){
        l_p = bme.readPressure() / 100.0f;
        l_h = bme.readHumidity();
      }

      // Evitamos sobreescribir con ceros si el sensor de polvo falló la lectura
      xSemaphoreTake(dataMutex, portMAX_DELAY);
      nh3_ppm = l_nh3;
      tempC = l_t; pres_hPa = l_p; humRH = l_h;
      if(senOk){ pm1_0 = l_pm1; pm2_5 = l_pm25; pm10 = l_pm10; }
      
      // Resetear el silencio si el estado volvió a normalidad
      if(currentLevel == NORMAL && buzzerMuted){
        buzzerMuted = false;
      }
      xSemaphoreGive(dataMutex);

      // Fusión de Datos
      float x[4] = { normPM1(pm1_0), normPM25(pm2_5), normPM10(pm10), normNH3(nh3_ppm) };

      histPM25[histIdx] = x[1];
      histNH3[histIdx]  = x[3];
      histIdx = (histIdx + 1) % WIN;
      if(histCount < WIN) histCount++;

      int cluster = -1; float dist = 0.0f;
      Level vK = voteKMeans(x, cluster, dist);
      Level vF = voteFlagsFusion(x, tempC, humRH, pres_hPa);
      Level vT = voteTrend();

      Level cLevel = majority(vK, vF, vT);
      String cCausa = String(causeLabel(x));
      String cEstado = String(levelName(cLevel));

      // Guardamos resultados globales protegidos
      xSemaphoreTake(dataMutex, portMAX_DELAY);
      currentLevel = cLevel;
      currentCausa = cCausa;
      currentEstado = cEstado;
      xSemaphoreGive(dataMutex);

      setLedByLevel(cLevel);
    }

    // 3) Refresco de Pantalla LCD (Cada 2000ms)
    if(now - lastScreenMs > 2000){
      lastScreenMs = now;
      screenIndex = (screenIndex + 1) % 4;
      
      // Tomamos fotos de las variables para enviarlas al LCD
      xSemaphoreTake(dataMutex, portMAX_DELAY);
      Level disp_lvl = currentLevel;
      String disp_causa = currentCausa;
      uint16_t disp_pm1 = pm1_0, disp_pm25 = pm2_5, disp_pm10 = pm10;
      float disp_nh3 = nh3_ppm, disp_t = tempC, disp_h = humRH;
      xSemaphoreGive(dataMutex);

      lcdShow(disp_lvl, disp_causa.c_str(), disp_pm1, disp_pm25, disp_pm10, disp_nh3, disp_t, disp_h);
    }

    // Liberar tiempo de CPU al Watchdog (Pausa de 20ms)
    vTaskDelay(pdMS_TO_TICKS(20)); 
  }
}

// =====================================================
// SEGURIDAD Y RUTINAS DEL SERVIDOR WEB
// =====================================================
bool is_authenticated() {
  if (server.hasHeader("Cookie")) {
    String cookie = server.header("Cookie");
    if (cookie.indexOf("SESSION_ID=1") != -1) return true;
  }
  return false;
}

void handleRoot() {
  if (!is_authenticated()) {
    server.sendHeader("Location", "/login");
    server.send(303);
    return;
  }
  server.send(200, "text/html", index_html);
}

void handleLoginGet() {
  if (is_authenticated()) {
    server.sendHeader("Location", "/");
    server.send(303);
    return;
  }
  server.send(200, "text/html", login_html);
}

void handleLoginPost() {
  if (server.hasArg("pass") && server.arg("pass") == www_pass) {
    server.sendHeader("Set-Cookie", "SESSION_ID=1; Path=/; HttpOnly");
    server.sendHeader("Location", "/");
    server.send(303);
  } else {
    server.sendHeader("Location", "/login?err=1");
    server.send(303);
  }
}

void handleLogout() {
  server.sendHeader("Set-Cookie", "SESSION_ID=0; expires=Thu, 01 Jan 1970 00:00:00 GMT; Path=/");
  server.sendHeader("Location", "/login");
  server.send(303);
}

void handleData() {
  if (!is_authenticated()) {
    server.send(401, "text/plain", "Unauthorized");
    return;
  }
  
  // Construir el JSON bloqueando el Mutex para asegurar datos consistentes
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  String json = "{";
  json += "\"pm1\":" + String(pm1_0) + ",";
  json += "\"pm25\":" + String(pm2_5) + ",";
  json += "\"pm10\":" + String(pm10) + ",";
  json += "\"nh3\":\"" + String(nh3_ppm, 2) + "\",";
  json += "\"t\":" + (isnan(tempC) ? "null" : String(tempC, 1)) + ",";
  json += "\"h\":" + (isnan(humRH) ? "null" : String(humRH, 1)) + ",";
  json += "\"p\":" + (isnan(pres_hPa) ? "null" : String(pres_hPa, 1)) + ",";
  json += "\"estado\":\"" + currentEstado + "\",";
  json += "\"causa\":\"" + currentCausa + "\",";
  json += "\"muted\":" + String(buzzerMuted ? "true" : "false");
  json += "}";
  xSemaphoreGive(dataMutex);

  server.send(200, "application/json", json);
}

void handleMute() {
  if (!is_authenticated()) {
    server.send(401, "text/plain", "Unauthorized");
    return;
  }
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  buzzerMuted = !buzzerMuted; 
  bool b_state = buzzerMuted;
  xSemaphoreGive(dataMutex);
  
  server.send(200, "text/plain", b_state ? "1" : "0");
}

// =====================================================
// reconectar mqtt
// =====================================================

void reconnectMQTT() {
  while (!mqttClient.connected()) {
    Serial.print("Conectando a MQTT... ");

    String clientId = "ESP32_Aire_";
    clientId += String((uint32_t)ESP.getEfuseMac(), HEX);

    if (mqttClient.connect(clientId.c_str())) {
      Serial.println("conectado");
    } else {
      Serial.print("fallo, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" reintentando en 3 segundos");
      delay(3000);
    }
  }
}

void publishSensorDataMQTT() {
  xSemaphoreTake(dataMutex, portMAX_DELAY);

  String json = "{";
  json += "\"pm1\":" + String(pm1_0) + ",";
  json += "\"pm25\":" + String(pm2_5) + ",";
  json += "\"pm10\":" + String(pm10) + ",";
  json += "\"nh3\":" + String(nh3_ppm, 2) + ",";
  json += "\"t\":" + (isnan(tempC) ? "null" : String(tempC, 1)) + ",";
  json += "\"h\":" + (isnan(humRH) ? "null" : String(humRH, 1)) + ",";
  json += "\"p\":" + (isnan(pres_hPa) ? "null" : String(pres_hPa, 1)) + ",";
  json += "\"estado\":\"" + currentEstado + "\",";
  json += "\"causa\":\"" + currentCausa + "\",";
  json += "\"muted\":" + String(buzzerMuted ? "true" : "false");
  json += "}";

  xSemaphoreGive(dataMutex);

  bool ok = mqttClient.publish(mqtt_topic, json.c_str());

  Serial.print("MQTT publicado: ");
  Serial.print(ok ? "OK | " : "ERROR | ");
  Serial.println(json);
}



// =====================================================
// SETUP 
// =====================================================
void setup(){
  Serial.begin(115200);
  delay(800);

  // Inicializar Mutex
  dataMutex = xSemaphoreCreateMutex();

  Wire.begin();
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Conectando WIFI");

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  

  Serial.println("\nWiFi conectado. IP: ");
  Serial.println(WiFi.localIP());
  mqttClient.setServer(mqtt_server, mqtt_port);
  

  server.collectHeaders(headerkeys, headerkeyssize);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/login", HTTP_GET, handleLoginGet);
  server.on("/login", HTTP_POST, handleLoginPost);
  server.on("/logout", HTTP_GET, handleLogout);
  server.on("/api/data", HTTP_GET, handleData);
  server.on("/api/mute", HTTP_POST, handleMute);
  server.begin();

  bme.begin(0x76) || bme.begin(0x77);
  dustSerial.begin(DUST_BAUD, SERIAL_8N1, DUST_RX, DUST_TX);
  dustSerial.setTimeout(1500);

  ledcAttach(LED_R, 5000, 8);
  ledcAttach(LED_G, 5000, 8);
  ledcAttach(LED_B, 5000, 8);
  pwmWriteRgb(0,0,0);

  pinMode(BUZZER_PIN, OUTPUT);
  buzzerWrite(false);

  for(int i=0;i<WIN;i++){ histPM25[i]=0; histNH3[i]=0; }
  
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("WiFi conectado");
  lcd.setCursor(0,1);
  lcd.print("Exitosamente");
  delay(1500);

  // Sonido de confirmacion de conexion WiFi
  playWifiConnectedBeep();

  String ipStr = WiFi.localIP().toString();
  String url = "http://" + ipStr;

  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print(url.substring(0, min(16, (int)url.length())));

  if (url.length() > 16) {
    lcd.setCursor(0,1);
    lcd.print(url.substring(16, min(32, (int)url.length())));
  }

  delay(10000);

  // Crear la tarea de hardware en el Core 0
  xTaskCreatePinnedToCore(
    hardwareLogicTask,   // Función de la tarea
    "TaskHardware",      // Nombre
    10000,               // Tamaño del Stack
    NULL,                // Parámetros
    1,                   // Prioridad
    &TaskHardware,       // Identificador
    0                    // Ejecutar en el Core 0
  );
}

// =====================================================
// LOOP PRINCIPAL (SOLO WEB)
// =====================================================
void loop(){
  server.handleClient();

  if (!mqttClient.connected()) {
    reconnectMQTT();
  }

  mqttClient.loop();

  unsigned long now = millis();
  if (now - lastMqttPublishMs >= MQTT_INTERVAL_MS) {
    lastMqttPublishMs = now;
    publishSensorDataMQTT();
  }

  delay(2);
}