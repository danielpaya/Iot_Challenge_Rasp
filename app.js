import { createClient } from "https://cdn.jsdelivr.net/npm/@supabase/supabase-js/+esm";

const SUPABASE_URL = "https://zbkyvafazeifrpcgmhrp.supabase.co";
const SUPABASE_PUBLISHABLE_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Inpia3l2YWZhemVpZnJwY2dtaHJwIiwicm9sZSI6InNlcnZpY2Vfcm9sZSIsImlhdCI6MTc3ODM1MDI5MiwiZXhwIjoyMDkzOTI2MjkyfQ.DSYSg0-sXZLBcZ3theWSX1KpJWL5UDJpGwPL2KCMF1M";

const supabase = createClient(SUPABASE_URL, SUPABASE_PUBLISHABLE_KEY);

const POLLING_INTERVAL_MS = 3000;
const HISTORY_LIMIT = 40;
const FILTER_LIMIT = 200;

const elements = {
  connectionStatus: document.getElementById("connectionStatus"),
  estado: document.getElementById("estado"),
  causa: document.getElementById("causa"),
  estadoCard: document.getElementById("estadoCard"),
  pm1: document.getElementById("pm1"),
  pm25: document.getElementById("pm25"),
  pm10: document.getElementById("pm10"),
  nh3: document.getElementById("nh3"),
  temperature: document.getElementById("temperature"),
  humidity: document.getElementById("humidity"),
  pressure: document.getElementById("pressure"),
  lastUpdate: document.getElementById("lastUpdate"),
  readingsTable: document.getElementById("readingsTable"),
  recordCount: document.getElementById("recordCount"),
  
  // Controls
  voiceToggle: document.getElementById("voiceToggle"),
  filterStatus: document.getElementById("filterStatus"),
  
  // AIRA
  airaSection: document.getElementById("airaSection"),
  airaRecommendation: document.getElementById("airaRecommendation"),
  airaTimestamp: document.getElementById("airaTimestamp"),
  airaTrigger: document.getElementById("airaTrigger"),
  airaAnalyzedState: document.getElementById("airaAnalyzedState"),
  airaAnalyzedCause: document.getElementById("airaAnalyzedCause"),
  airaModel: document.getElementById("airaModel"),
  requestAiraBtn: document.getElementById("requestAiraBtn"),
  airaRequestStatus: document.getElementById("airaRequestStatus"),
  
  // Filters
  filterDate: document.getElementById("filterDate"),
  filterStartDate: document.getElementById("filterStartDate"),
  filterEndDate: document.getElementById("filterEndDate"),
  filterStartTime: document.getElementById("filterStartTime"),
  filterEndTime: document.getElementById("filterEndTime"),
  filterState: document.getElementById("filterState"),
  filterVariable: document.getElementById("filterVariable"),
  btnApplyFilters: document.getElementById("btnApplyFilters"),
  btnClearFilters: document.getElementById("btnClearFilters"),
};

let pmChart;

// State Variables
let filtersActive = false;
let voiceAlertsEnabled = false;
let lastSpokenDangerId = null;

// --- Formatting Helpers ---
function formatValue(value, decimals = 1) {
  if (value === null || value === undefined || Number.isNaN(Number(value))) {
    return "--";
  }
  return Number(value).toFixed(decimals);
}

function formatDate(value) {
  if (!value) return "--";
  return new Date(value).toLocaleString("es-CO", {
    dateStyle: "short",
    timeStyle: "medium",
  });
}

function getEstadoClass(estado) {
  const clean = String(estado || "").toLowerCase();
  if (clean.includes("peligro")) return "estado-peligro";
  if (clean.includes("precauc")) return "estado-precaucion";
  return "estado-normal";
}

function setConnectionStatus(text, ok = true) {
  elements.connectionStatus.textContent = text;
  elements.connectionStatus.className = ok
    ? "status-pill status-ok"
    : "status-pill status-error";
}

// --- Voice Alert Logic ---
function initVoiceAlerts() {
  if (!("speechSynthesis" in window)) {
    elements.voiceToggle.textContent = "Voz no soportada";
    elements.voiceToggle.disabled = true;
    return;
  }

  elements.voiceToggle.addEventListener("click", () => {
    voiceAlertsEnabled = !voiceAlertsEnabled;
    
    if (voiceAlertsEnabled) {
      elements.voiceToggle.textContent = "Alertas de voz activadas";
      elements.voiceToggle.classList.add("active");
      
      // Test speech on activation
      const msg = new SpeechSynthesisUtterance("Alertas de voz activadas.");
      msg.lang = "es-ES";
      window.speechSynthesis.speak(msg);
    } else {
      elements.voiceToggle.textContent = "Activar alertas de voz";
      elements.voiceToggle.classList.remove("active");
      window.speechSynthesis.cancel();
    }
  });
}

function handleVoiceAlert(latest) {
  const cleanEstado = String(latest.estado || "").toLowerCase();
  
  if (cleanEstado.includes("peligro")) {
    // Add critical style
    elements.estadoCard.classList.add("estado-peligro-critico");
    
    // Check if we already spoke for this ID
    if (voiceAlertsEnabled && latest.id !== lastSpokenDangerId) {
      lastSpokenDangerId = latest.id;
      
      const msg = new SpeechSynthesisUtterance(
        "Alerta de peligro. Se detectaron niveles críticos de calidad del aire. Revise el dashboard."
      );
      msg.lang = "es-ES";
      window.speechSynthesis.speak(msg);
    }
  } else {
    // Not in danger anymore
    elements.estadoCard.classList.remove("estado-peligro-critico");
    lastSpokenDangerId = null;
  }
}

// --- DOM Updating ---
function updateCards(latest) {
  if (!latest) return;

  elements.estado.textContent = latest.estado || "--";
  elements.estado.className = getEstadoClass(latest.estado);

  elements.causa.textContent = latest.causa
    ? `Causa principal: ${latest.causa}`
    : "Sin causa reportada";

  elements.pm1.textContent = formatValue(latest.pm1, 0);
  elements.pm25.textContent = formatValue(latest.pm25, 0);
  elements.pm10.textContent = formatValue(latest.pm10, 0);
  elements.nh3.textContent = formatValue(latest.nh3, 2);
  elements.temperature.textContent = formatValue(latest.temperature, 1);
  elements.humidity.textContent = formatValue(latest.humidity, 1);
  elements.pressure.textContent = formatValue(latest.pressure, 1);

  elements.lastUpdate.textContent = `Última actualización: ${formatDate(latest.timestamp_utc)}`;
}

function updateTable(rows) {
  elements.recordCount.textContent = `Mostrando ${rows.length} registro(s)${filtersActive ? " filtrado(s)" : ""}`;

  if (!rows.length) {
    elements.readingsTable.innerHTML = `
      <tr>
        <td colspan="10">No hay datos disponibles.</td>
      </tr>
    `;
    return;
  }

  elements.readingsTable.innerHTML = rows
    .map((row) => {
      const estadoClass = getEstadoClass(row.estado);
      return `
        <tr>
          <td>${formatDate(row.timestamp_utc)}</td>
          <td>${formatValue(row.pm1, 0)}</td>
          <td>${formatValue(row.pm25, 0)}</td>
          <td>${formatValue(row.pm10, 0)}</td>
          <td>${formatValue(row.nh3, 2)}</td>
          <td>${formatValue(row.temperature, 1)}</td>
          <td>${formatValue(row.humidity, 1)}</td>
          <td>${formatValue(row.pressure, 1)}</td>
          <td class="${estadoClass}">${row.estado || "--"}</td>
          <td>${row.causa || "--"}</td>
        </tr>
      `;
    })
    .join("");
}

function updateChart(rows) {
  const orderedRows = [...rows].reverse();

  const labels = orderedRows.map((row) =>
    new Date(row.timestamp_utc).toLocaleTimeString("es-CO", {
      hour: "2-digit",
      minute: "2-digit",
      second: "2-digit",
    })
  );

  const selectedVar = elements.filterVariable.value;
  let datasets = [];

  if (selectedVar === "Todos") {
    datasets = [
      { label: "PM2.5", data: orderedRows.map(r => r.pm25), tension: 0.3, borderColor: '#3b82f6', backgroundColor: '#3b82f6' },
      { label: "PM10", data: orderedRows.map(r => r.pm10), tension: 0.3, borderColor: '#f59e0b', backgroundColor: '#f59e0b' }
    ];
  } else {
    const labelMap = { 
      pm1: 'PM1.0', pm25: 'PM2.5', pm10: 'PM10', 
      nh3: 'NH3', temperature: 'Temperatura', 
      humidity: 'Humedad', pressure: 'Presión' 
    };
    datasets = [
      { label: labelMap[selectedVar], data: orderedRows.map(r => r[selectedVar]), tension: 0.3, borderColor: '#22c55e', backgroundColor: '#22c55e' }
    ];
  }

  if (!pmChart) {
    const ctx = document.getElementById("pmChart");
    pmChart = new Chart(ctx, {
      type: "line",
      data: { labels, datasets },
      options: {
        responsive: true,
        plugins: {
          legend: { labels: { color: "#f8fafc" } },
        },
        scales: {
          x: { ticks: { color: "#94a3b8" }, grid: { color: "#334155" } },
          y: { ticks: { color: "#94a3b8" }, grid: { color: "#334155" } },
        },
      },
    });
    return;
  }

  pmChart.data.labels = labels;
  pmChart.data.datasets = datasets;
  pmChart.update();
}

// --- AIRA Logic ---
async function fetchLatestAiraRecommendation() {
  try {
    const { data, error } = await supabase
      .from("ai_recommendations")
      .select("*")
      .order("timestamp_utc", { ascending: false })
      .limit(1);

    if (error) throw error;

    if (!data || data.length === 0) {
      elements.airaRecommendation.textContent = "AIRA aún no ha generado recomendaciones.";
      elements.airaTimestamp.textContent = "Última recomendación: --";
      elements.airaTrigger.textContent = "Activación: --";
      elements.airaAnalyzedState.textContent = "Estado analizado: --";
      elements.airaAnalyzedCause.textContent = "Causa: --";
      elements.airaModel.textContent = "Modelo: --";
      elements.airaRequestStatus.textContent = "AIRA lista.";
      elements.airaSection.classList.remove("aira-danger");
      return;
    }

    const latest = data[0];
    elements.airaRecommendation.textContent = latest.recommendation || "Sin recomendación.";
    elements.airaTimestamp.textContent = `Última recomendación: ${formatDate(latest.timestamp_utc)}`;
    
    let triggerText = latest.trigger_source;
    if (triggerText === "automatic_peligro") triggerText = "Automática por estado Peligro";
    else if (triggerText === "manual_dashboard") triggerText = "Manual desde dashboard";
    
    elements.airaTrigger.textContent = `Activación: ${triggerText}`;
    elements.airaAnalyzedState.textContent = `Estado analizado: ${latest.estado || "--"}`;
    elements.airaAnalyzedCause.textContent = `Causa: ${latest.causa || "--"}`;
    elements.airaModel.textContent = `Modelo: ${latest.model || "--"}`;

    if (latest.trigger_source === "automatic_peligro") {
      elements.airaSection.classList.add("aira-danger");
      elements.airaRequestStatus.textContent = "Recomendación generada automáticamente por condición de Peligro.";
    } else {
      elements.airaSection.classList.remove("aira-danger");
      // Only reset status to 'lista' if it's not currently pending
      if (elements.airaRequestStatus.textContent !== "Solicitud enviada. Esperando respuesta de la Raspberry...") {
        elements.airaRequestStatus.textContent = "AIRA lista.";
      }
    }

  } catch (error) {
    console.error("Error leyendo AIRA:", error);
    elements.airaRequestStatus.textContent = "No se pudo leer la recomendación de AIRA.";
  }
}

async function requestAiraRecommendation() {
  try {
    elements.requestAiraBtn.disabled = true;
    elements.requestAiraBtn.textContent = "Solicitando AIRA...";

    // Anti-spam check (60 seconds)
    const sixtySecondsAgo = new Date(Date.now() - 60000).toISOString();
    const { data: recentRequests, error: checkError } = await supabase
      .from("ai_requests")
      .select("created_at")
      .eq("request_type", "manual")
      .eq("status", "pending")
      .gte("created_at", sixtySecondsAgo)
      .limit(1);

    if (checkError) throw checkError;

    if (recentRequests && recentRequests.length > 0) {
      elements.airaRequestStatus.textContent = "Ya hay una solicitud pendiente para AIRA.";
      return;
    }

    elements.airaRequestStatus.textContent = "Solicitud enviada. Esperando respuesta de la Raspberry...";

    const { error: insertError } = await supabase.from("ai_requests").insert({
      request_type: "manual",
      status: "pending",
      source: "dashboard"
    });

    if (insertError) throw insertError;
    
    console.log("Solicitud AIRA enviada");

    // Fetch progressively as processing takes time
    [5000, 10000, 15000].forEach(delay => {
      setTimeout(() => {
        fetchLatestAiraRecommendation();
      }, delay);
    });

  } catch (error) {
    console.error("Error solicitando AIRA:", error);
    elements.airaRequestStatus.textContent = "Error solicitando recomendación de AIRA.";
  } finally {
    // Cooldown visual
    setTimeout(() => {
      elements.requestAiraBtn.disabled = false;
      elements.requestAiraBtn.textContent = "Solicitar recomendación de AIRA";
    }, 15000);
  }
}

// --- Data Fetching ---

// Polls only 1 record to keep status cards and voice alerts active
async function pollLatestData() {
  const { data, error } = await supabase
    .from("readings")
    .select("*")
    .order("timestamp_utc", { ascending: false })
    .limit(1);

  if (error) {
    console.error("Error fetching latest:", error);
    setConnectionStatus("Error leyendo Supabase", false);
    return;
  }

  setConnectionStatus("Datos en vivo", true);
  
  if (data && data.length > 0) {
    const latest = data[0];
    updateCards(latest);
    handleVoiceAlert(latest);
  }
}

// Fetches history (filtered or live) for the table and chart
async function fetchHistoricalData() {
  let query = supabase.from("readings").select("*").order("timestamp_utc", { ascending: false });

  if (filtersActive) {
    query = query.limit(FILTER_LIMIT);

    // Filter by single date
    if (elements.filterDate.value) {
      const localStart = new Date(elements.filterDate.value + "T00:00:00");
      const localEnd = new Date(elements.filterDate.value + "T23:59:59.999");
      query = query.gte("timestamp_utc", localStart.toISOString());
      query = query.lte("timestamp_utc", localEnd.toISOString());
    } 
    // Or filter by date range
    else if (elements.filterStartDate.value && elements.filterEndDate.value) {
      const localStart = new Date(elements.filterStartDate.value + "T00:00:00");
      const localEnd = new Date(elements.filterEndDate.value + "T23:59:59.999");
      query = query.gte("timestamp_utc", localStart.toISOString());
      query = query.lte("timestamp_utc", localEnd.toISOString());
    }

    // Filter by State
    if (elements.filterState.value !== "Todos") {
      // Supabase EQ requires exact match, but our values might have accents in DB or be slightly different.
      // Assuming 'Normal', 'Precaución', 'Peligro' mapping.
      const stateVal = elements.filterState.value === "Precaucion" ? "Precaución" : elements.filterState.value;
      query = query.eq("estado", stateVal);
    }
  } else {
    query = query.limit(HISTORY_LIMIT);
  }

  const { data, error } = await query;

  if (error) {
    console.error("Error fetching historical:", error);
    return;
  }

  let finalData = data || [];

  // Filter by Time Range in JavaScript
  if (filtersActive && (elements.filterStartTime.value || elements.filterEndTime.value)) {
    finalData = finalData.filter(row => {
      const d = new Date(row.timestamp_utc);
      const hhmm = d.getHours().toString().padStart(2, '0') + ":" + d.getMinutes().toString().padStart(2, '0');
      
      let valid = true;
      if (elements.filterStartTime.value && hhmm < elements.filterStartTime.value) valid = false;
      if (elements.filterEndTime.value && hhmm > elements.filterEndTime.value) valid = false;
      return valid;
    });
  }

  updateTable(finalData);
  updateChart(finalData);
}

// Main polling loop manager
async function pollingLoop() {
  await pollLatestData();
  await fetchLatestAiraRecommendation();
  
  if (!filtersActive) {
    await fetchHistoricalData();
  }
}

// --- Filter Events ---
function applyFilters() {
  filtersActive = true;
  elements.filterStatus.style.display = "inline-block";
  fetchHistoricalData();
}

function clearFilters() {
  filtersActive = false;
  elements.filterStatus.style.display = "none";
  
  elements.filterDate.value = "";
  elements.filterStartDate.value = "";
  elements.filterEndDate.value = "";
  elements.filterStartTime.value = "";
  elements.filterEndTime.value = "";
  elements.filterState.value = "Todos";
  elements.filterVariable.value = "Todos";
  
  fetchHistoricalData();
}

elements.btnApplyFilters.addEventListener("click", applyFilters);
elements.btnClearFilters.addEventListener("click", clearFilters);
elements.requestAiraBtn.addEventListener("click", requestAiraRecommendation);

// Initial Load
initVoiceAlerts();
pollingLoop();
setInterval(pollingLoop, POLLING_INTERVAL_MS);