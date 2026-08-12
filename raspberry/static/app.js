
const $ = (id) => document.getElementById(id);
let updatingRelays = false;

function ago(iso) {
  if (!iso) return "Sin actualización";

  const seconds = Math.max(
    0,
    Math.floor(
      (Date.now() - new Date(iso).getTime()) / 1000
    )
  );

  if (seconds < 5) return "Ahora mismo";
  if (seconds < 60) return `Hace ${seconds} s`;

  const minutes = Math.floor(seconds / 60);

  if (minutes < 60) return `Hace ${minutes} min`;

  const hours = Math.floor(minutes / 60);

  if (hours < 24) return `Hace ${hours} h`;

  return `Hace ${Math.floor(hours / 24)} d`;
}

function number(value, decimals = 1) {
  if (
    value === null ||
    value === undefined ||
    Number.isNaN(Number(value))
  ) {
    return "--";
  }

  return Number(value).toFixed(decimals);
}

function updateDashboard(data) {
  const serialOnline = Boolean(data.serial?.connected);

  $("serialDot").classList.toggle(
    "online",
    serialOnline
  );

  $("serialText").textContent =
    serialOnline
      ? `UART conectada · ${data.serial.port}`
      : `UART desconectada · ${data.serial?.port || ""}`;

  // Agua
  const level = data.water?.level_percent;

  $("waterLevel").textContent =
    number(level, 0);

  $("waterBar").style.width =
    `${Math.max(0, Math.min(100, Number(level) || 0))}%`;

  $("flow1").textContent =
    number(data.water?.flow1_lmin, 2);

  $("flow2").textContent =
    number(data.water?.flow2_lmin, 2);

  $("lit1").textContent =
    number(data.water?.total1_liters, 3);

  $("lit2").textContent =
    number(data.water?.total2_liters, 3);

  // Ultrasonicos
  $("us1Distance").textContent =
    number(data.ultrasonics?.us1_cm, 1);

  $("us1Card").textContent =
    number(data.ultrasonics?.us1_cm, 1);

  $("us2Card").textContent =
    number(data.ultrasonics?.us2_cm, 1);

  // Presencia
  const presence = data.presence || {};

  $("presenceState").textContent =
    presence.detected
      ? "Presencia detectada"
      : "Sin presencia";

  $("presenceTime").textContent =
    presence.detected
      ? `Detectada ${ago(presence.updated_at).toLowerCase()}`
      : `Última detección: ${ago(presence.last_detected_at)}`;

  // Cerradura
  const lock = data.lock || {};

  $("lockState").textContent =
    lock.open ? "Abierta" : "Cerrada";

  $("lockSource").textContent =
    lock.source || "Sin datos";

  $("lockTime").textContent =
    ago(lock.updated_at);

  // Password
  const password = data.password || {};

  if (password.result === "correct") {
    $("passwordState").textContent =
      "Contraseña correcta";

    $("passwordTime").textContent =
      `Último acceso: ${ago(password.updated_at)}`;
  } else if (password.result === "incorrect") {
    $("passwordState").textContent =
      "Contraseña incorrecta";

    $("passwordTime").textContent =
      `Último intento: ${ago(password.updated_at)}`;
  } else {
    $("passwordState").textContent =
      "Sin intentos";

    $("passwordTime").textContent =
      "Esperando teclado 4x4";
  }

  // ENS160
  $("aqi").textContent =
    number(data.air?.aqi, 0);

  $("tvoc").textContent =
    number(data.air?.tvoc_ppb, 0);

  $("eco2").textContent =
    number(data.air?.eco2_ppm, 0);

  // Relevadores
  updatingRelays = true;

  for (let i = 1; i <= 8; i++) {
    const state = Boolean(
      data.relays?.[String(i)]
    );

    $(`relayToggle${i}`).checked = state;
    $(`relayState${i}`).textContent =
      state ? "ON" : "OFF";
  }

  updatingRelays = false;

  // Cámaras
  for (const camera of data.cameras || []) {
    const element =
      $(`cameraStatus${camera.index}`);

    if (element) {
      element.textContent =
        camera.connected
          ? "En línea"
          : "Sin señal";
    }
  }
}

async function refreshState() {
  try {
    const response = await fetch(
      "/api/state",
      { cache: "no-store" }
    );

    if (!response.ok) {
      throw new Error(
        `HTTP ${response.status}`
      );
    }

    updateDashboard(
      await response.json()
    );
  } catch (error) {
    $("serialDot").classList.remove("online");
    $("serialText").textContent =
      "Servidor sin respuesta";
  }
}

async function setRelay(relay, state) {
  $("commandFeedback").textContent =
    `Cambiando R${relay}...`;

  try {
    const response = await fetch(
      `/api/relay/${relay}`,
      {
        method: "POST",
        headers: {
          "Content-Type": "application/json"
        },
        body: JSON.stringify({ state })
      }
    );

    const result =
      await response.json();

    if (!response.ok || !result.ok) {
      throw new Error(
        result.error || "No se pudo cambiar"
      );
    }

    $("commandFeedback").textContent =
      `R${relay} ${state ? "ON" : "OFF"}`;
  } catch (error) {
    $("commandFeedback").textContent =
      `Error: ${error.message}`;
  }

  setTimeout(refreshState, 100);
}

async function setLock(state) {
  $("commandFeedback").textContent =
    "Enviando comando de cerradura...";

  try {
    const response = await fetch(
      "/api/lock",
      {
        method: "POST",
        headers: {
          "Content-Type": "application/json"
        },
        body: JSON.stringify({ state })
      }
    );

    const result =
      await response.json();

    if (!response.ok || !result.ok) {
      throw new Error(
        "UART con ESP32 no disponible"
      );
    }

    $("commandFeedback").textContent =
      `Cerradura: ${state}`;
  } catch (error) {
    $("commandFeedback").textContent =
      `Error: ${error.message}`;
  }
}

async function resetFlow(which) {
  try {
    await fetch(
      `/api/flow/reset/${which}`,
      { method: "POST" }
    );
  } finally {
    setTimeout(refreshState, 150);
  }
}

for (let i = 1; i <= 8; i++) {
  $(`relayToggle${i}`).addEventListener(
    "change",
    (event) => {
      if (!updatingRelays) {
        setRelay(
          i,
          event.target.checked
        );
      }
    }
  );
}

document
  .querySelectorAll("[data-lock]")
  .forEach((button) => {
    button.addEventListener(
      "click",
      () => setLock(button.dataset.lock)
    );
  });

document
  .querySelectorAll("[data-reset-flow]")
  .forEach((button) => {
    button.addEventListener(
      "click",
      () => resetFlow(
        button.dataset.resetFlow
      )
    );
  });

function updateCameraClock() {
  const value =
    new Date().toLocaleTimeString();

  $("cameraTime1").textContent = value;
  $("cameraTime2").textContent = value;
}

updateCameraClock();
setInterval(updateCameraClock, 1000);

refreshState();
setInterval(refreshState, 1000);
