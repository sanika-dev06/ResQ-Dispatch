/* ResQ Dispatch - frontend logic.
   Same screens and behaviour as the original single-file demo, but data now lives
   on the C++ backend (REST API) instead of localStorage:
   - accounts, sessions, profile and notifications  -> /api/auth, /api/profile, /api/notifications
   - emergencies, priority queue, fleet, dispatching -> /api/state, /api/emergencies, /api/dispatch ...
   No credentials are stored in this file. */

const TOKEN_KEY = "resqDispatchToken";
let authToken = null;
try { authToken = sessionStorage.getItem(TOKEN_KEY); } catch (error) { /* blocked storage: stay signed in for this page only */ }
let currentUser = { name: "", username: "", email: "", role: "Dispatcher", isAdmin: false, createdAt: "" };
let pollTimer = null;

/* ---------------------------------------------------------------- API helper */
async function api(path, method = "GET", body) {
  const options = { method, headers: {} };
  if (authToken) options.headers["Authorization"] = "Bearer " + authToken;
  if (body !== undefined) { options.headers["Content-Type"] = "application/json"; options.body = JSON.stringify(body); }
  let response;
  try { response = await fetch(path, options); }
  catch (error) { throw new Error("Cannot reach the ResQ server. Make sure the backend is running and open http://localhost:8080 in the browser."); }
  let data = null;
  try { data = await response.json(); } catch (error) { /* non-JSON error page */ }
  if (response.status === 401 && authToken && path.indexOf("/api/auth/login") !== 0) {
    sessionExpired();
    throw new Error((data && data.error) || "Your session has expired. Please sign in again.");
  }
  if (!response.ok) throw new Error((data && data.error) || ("Request failed (" + response.status + ")."));
  return data;
}
function setToken(token) {
  authToken = token;
  try { if (token) sessionStorage.setItem(TOKEN_KEY, token); else sessionStorage.removeItem(TOKEN_KEY); } catch (error) {}
}
function sessionExpired() {
  setToken(null);
  stopPolling();
  showLoginScreen();
  setAuthError("loginError", "Your session has expired. Please sign in again.");
}

/* ---------------------------------------------------------------- helpers */
function escapeHTML(value) {
  return String(value ?? "").replace(/[&<>"']/g, char => ({
    "&":"&amp;", "<":"&lt;", ">":"&gt;", '"':"&quot;", "'":"&#39;"
  })[char]);
}
const escapeHtml = escapeHTML;
function timeAgo(iso) {
  const then = new Date(iso);
  if (!iso || Number.isNaN(then.getTime())) return "";
  const minutes = Math.floor((Date.now() - then.getTime()) / 60000);
  if (minutes < 1) return "Just now";
  if (minutes < 60) return minutes + (minutes === 1 ? " min ago" : " mins ago");
  const hours = Math.floor(minutes / 60);
  if (hours < 24) return hours + (hours === 1 ? " hr ago" : " hrs ago");
  return then.toLocaleDateString();
}
function clockTime(iso) {
  const when = new Date(iso);
  return Number.isNaN(when.getTime()) ? "" : when.toLocaleTimeString([], {hour:"2-digit", minute:"2-digit"});
}

/* ---------------------------------------------------------------- auth screens */
function setAuthError(id, message) {
  const el = document.getElementById(id); el.textContent = message; el.style.display = "block";
}
function clearAuthErrors() {
  ["loginError", "signupError"].forEach(id => { const el = document.getElementById(id); if (el) { el.textContent = ""; el.style.display = "none"; } });
}
function showAuthMode(mode) {
  clearAuthErrors();
  const signIn = mode === "signin";
  document.getElementById("signInPanel").style.display = signIn ? "block" : "none";
  document.getElementById("signUpPanel").style.display = signIn ? "none" : "block";
  document.getElementById("signInTab").className = signIn ? "btn red" : "btn light";
  document.getElementById("createTab").className = signIn ? "btn light" : "btn red";
}
function showLoginScreen() {
  document.getElementById("appRoot").style.display = "none";
  document.getElementById("loginScreen").style.display = "flex";
  document.getElementById("loginPassword").value = "";
  showAuthMode("signin");
}
function enterApp(user) {
  const isAdmin = user && user.role === "admin";
  currentUser = {
    name: user.name || "", username: user.username || "", email: user.email || "",
    role: isAdmin ? "Admin / Dispatcher" : "Dispatcher", isAdmin, createdAt: user.createdAt || ""
  };
  document.getElementById("loginScreen").style.display = "none";
  document.getElementById("appRoot").style.display = "flex";
  document.getElementById("profileName").textContent = currentUser.name || currentUser.username || "Dispatcher";
  document.getElementById("profileAvatar").textContent = (currentUser.name || currentUser.username || "D").trim().charAt(0).toUpperCase();
  updateProfileDetails();
  refresh();
  loadNotifications();
  startPolling();
}
function startPolling() {
  stopPolling();
  // Keeps several dispatchers' screens in sync (and the "x mins ago" labels fresh).
  pollTimer = setInterval(() => {
    if (!authToken || document.hidden) return;
    refresh();
    loadNotifications();
  }, 5000);
}
function stopPolling() { if (pollTimer) { clearInterval(pollTimer); pollTimer = null; } }

/* ---------------------------------------------------------------- profile */
function updateProfileDetails() {
  const name = currentUser.name || currentUser.username || "Dispatcher";
  const username = currentUser.username || "—";
  const role = currentUser.role || "Dispatcher";
  const initial = name.trim().charAt(0).toUpperCase() || "D";
  document.getElementById("profileModalName").textContent = name;
  const fullName = document.getElementById("profileFullName"); if (fullName) fullName.textContent = name;
  document.getElementById("profileUsername").textContent = username;
  document.getElementById("editProfileName").value = name;
  document.getElementById("editProfileEmail").value = currentUser.email || "";
  document.getElementById("profileModalRole").textContent = role;
  document.getElementById("profileRole").textContent = role;
  document.getElementById("profileLargeAvatar").textContent = initial;
  const created = currentUser.createdAt ? new Date(currentUser.createdAt) : null;
  document.getElementById("profileCreated").textContent = username === "admin"
    ? "System account"
    : (created && !Number.isNaN(created.getTime()) ? created.toLocaleDateString() : "Current session");
}
function openProfile() {
  updateProfileDetails();
  const modal = document.getElementById("profileModal");
  modal.classList.add("open");
  modal.querySelector(".profile-close").focus();
}
async function saveProfile() {
  const nameField = document.getElementById("editProfileName");
  const emailField = document.getElementById("editProfileEmail");
  const name = nameField.value.trim();
  const email = emailField.value.trim();
  const message = document.getElementById("profileSaveMessage");
  const showError = text => { message.style.display = "block"; message.style.background = "#fff0f1"; message.style.color = "#a81f30"; message.textContent = text; };
  if (!name) { showError("Please enter your full name."); nameField.focus(); return; }
  if (email && !emailField.checkValidity()) { showError("Please enter a valid email address."); emailField.focus(); return; }
  try {
    const data = await api("/api/profile", "PUT", { name, email });
    currentUser.name = data.user.name; currentUser.email = data.user.email;
    document.getElementById("profileName").textContent = currentUser.name;
    document.getElementById("profileAvatar").textContent = currentUser.name.charAt(0).toUpperCase();
    updateProfileDetails();
    message.style.display = "block"; message.style.background = "#e2f8ed"; message.style.color = "#087b4e";
    message.textContent = "Profile updated successfully.";
    loadNotifications();
  } catch (error) { showError(error.message); }
}
function closeProfile() {
  document.getElementById("profileModal").classList.remove("open");
  const trigger = document.getElementById("profileAvatar"); if (trigger) trigger.focus();
}

/* ---------------------------------------------------------------- notifications (stored on the server, per user) */
let notifications = [];
async function loadNotifications() {
  if (!authToken) return;
  try {
    const data = await api("/api/notifications");
    notifications = Array.isArray(data.notifications) ? data.notifications : [];
    updateNotificationBadge();
    if (document.getElementById("notificationsModal").classList.contains("open")) renderNotifications();
  } catch (error) { /* keep the last list; the next poll retries */ }
}
function updateNotificationBadge() {
  const dot = document.getElementById("notificationDot");
  if (dot) dot.style.display = notifications.some(item => !item.read) ? "block" : "none";
}
function renderNotifications() {
  const list = document.getElementById("notificationList");
  if (!notifications.length) { list.innerHTML = '<p style="font-size:12px;color:#64728a;padding:12px 0">You are all caught up. No notifications.</p>'; return; }
  list.innerHTML = notifications.map(item => `<div class="notification-item ${item.read ? "" : "unread"}"><div class="notification-icon">${escapeHtml(String(item.icon || "•"))}</div><div><strong>${escapeHtml(item.title)}</strong><p>${escapeHtml(item.message)} · ${(item.time && !Number.isNaN(new Date(item.time).getTime())) ? new Date(item.time).toLocaleString() : "Just now"}</p></div></div>`).join("");
}
function openNotifications() {
  renderNotifications();
  document.getElementById("notificationsModal").classList.add("open");
  updateNotificationBadge();
  loadNotifications();
}
function closeNotifications() { document.getElementById("notificationsModal").classList.remove("open"); }
async function markNotificationsRead() {
  try {
    const data = await api("/api/notifications/read-all", "POST");
    notifications = data.notifications || [];
    updateNotificationBadge();
    renderNotifications();
    showNotificationActionMessage("All notifications marked as read.");
  } catch (error) { showNotificationActionMessage(error.message); }
}
async function clearNotifications() {
  try {
    const data = await api("/api/notifications", "DELETE");
    notifications = data.notifications || [];
    updateNotificationBadge();
    renderNotifications();
    showNotificationActionMessage("All notifications cleared.");
  } catch (error) { showNotificationActionMessage(error.message); }
}
function showNotificationActionMessage(message) {
  let status = document.getElementById("notificationActionStatus");
  if (!status) {
    status = document.createElement("p");
    status.id = "notificationActionStatus";
    status.setAttribute("role", "status");
    status.style.cssText = "font-size:11px;color:#087b4e;margin:10px 0 0;min-height:16px";
    const list = document.getElementById("notificationList");
    list.insertAdjacentElement("afterend", status);
  }
  status.textContent = message;
}
document.getElementById("markAllReadBtn").addEventListener("click", markNotificationsRead);
document.getElementById("clearAllNotificationsBtn").addEventListener("click", clearNotifications);
document.addEventListener("keydown", function(event) { if (event.key === "Escape") { closeNotifications(); closeProfile(); } });

/* ---------------------------------------------------------------- sign in / sign up / sign out */
async function logout() {
  try { await api("/api/auth/logout", "POST"); } catch (error) { /* token may already be invalid */ }
  setToken(null);
  stopPolling();
  emergencies = []; fleet = []; history = []; queueOrder = []; notifications = [];
  showLoginScreen();
}
async function handleSignIn(event) {
  if (event) event.preventDefault();
  clearAuthErrors();
  const usernameField = document.getElementById("loginUsername");
  const passwordField = document.getElementById("loginPassword");
  const username = (usernameField.value || "").trim();
  const password = passwordField.value || "";
  if (!username || !password) {
    setAuthError("loginError", "Please enter both your username/email and password.");
    if (!username) usernameField.focus(); else passwordField.focus();
    return;
  }
  try {
    const data = await api("/api/auth/login", "POST", { username, password });
    setToken(data.token);
    enterApp(data.user);
  } catch (error) {
    setAuthError("loginError", error.message);
    passwordField.focus();
  }
}
const loginForm = document.getElementById("loginForm");
loginForm.addEventListener("submit", handleSignIn);
document.getElementById("signInButton").addEventListener("click", function(event) {
  event.preventDefault();
  if (loginForm.reportValidity()) handleSignIn(event);
});
async function handleCreateAccount() {
  clearAuthErrors();
  const form = document.getElementById("signupForm");
  if (!form.reportValidity()) return;
  const name = document.getElementById("signupName").value.trim();
  const username = document.getElementById("signupUsername").value.trim();
  const email = document.getElementById("signupEmail").value.trim().toLowerCase();
  const password = document.getElementById("signupPassword").value;
  const confirm = document.getElementById("signupConfirm").value;
  if (password !== confirm) { setAuthError("signupError", "Passwords do not match. Please try again."); return; }
  try {
    const data = await api("/api/auth/signup", "POST", { name, username, email, password });
    form.reset();
    setToken(data.token);
    enterApp(data.user);
  } catch (error) { setAuthError("signupError", error.message); }
}
document.getElementById("signupForm").addEventListener("submit", function(event) {
  event.preventDefault();
  handleCreateAccount();
});

/* ---------------------------------------------------------------- dashboard state (from the server) */
let emergencies = [];
let fleet = [];
let history = [];
let queueOrder = [];
let selectedPriority = "High";
let dispatchedCount = 0;

async function refresh() {
  if (!authToken) return;
  try {
    const state = await api("/api/state");
    emergencies = state.emergencies || [];
    fleet = state.fleet || [];
    history = state.history || [];
    queueOrder = state.queue || [];
    dispatchedCount = state.stats ? state.stats.dispatchedCount : 0;
    draw();
  } catch (error) { /* offline or signed out: keep what is on screen */ }
}
function showPage(name) {
  document.querySelectorAll(".page").forEach(p => p.classList.remove("active"));
  const page = document.getElementById("page-" + name);
  if (!page) return;
  page.classList.add("active");
  document.querySelectorAll(".nav").forEach(b => b.classList.toggle("active", b.dataset.page === name));
  const title = {
    dashboard:"Emergency Operations Dashboard",
    new:"Report a New Emergency",
    queue:"Priority Queue",
    ambulances:"Ambulance Dispatch",
    history:"Dispatch History",
    dsa:"DSA Concepts"
  }[name] || "ResQ Dispatch";
  document.getElementById("topTitle").textContent = title;
  document.getElementById("crumb").textContent = title;
  render();
}
document.querySelectorAll(".nav").forEach(button => {
  button.addEventListener("click", () => showPage(button.dataset.page));
});
document.querySelectorAll(".choice").forEach(button => {
  button.addEventListener("click", () => {
    document.querySelectorAll(".choice").forEach(item => item.classList.remove("selected"));
    button.classList.add("selected");
    selectedPriority = button.dataset.priority;
    document.getElementById("priority").value = selectedPriority;
  });
});
document.getElementById("form").addEventListener("submit", async event => {
  event.preventDefault();
  const form = event.target;
  const location = document.getElementById("location").value.trim();
  if (!location) {
    document.getElementById("location").focus();
    toast("Please enter the patient location.");
    return;
  }
  try {
    await api("/api/emergencies", "POST", {
      type: document.getElementById("type").value,
      priority: selectedPriority,
      location,
      patient: document.getElementById("patient").value.trim(),
      details: document.getElementById("details").value.trim()
    });
  } catch (error) { toast(error.message); return; }
  form.reset();
  selectedPriority = "High";
  document.getElementById("priority").value = "High";
  document.querySelectorAll(".choice").forEach(item => item.classList.toggle("selected", item.dataset.priority === "High"));
  await refresh();
  loadNotifications();
  toast("Emergency added to the priority queue.");
  showPage("queue");
});
function badge(priority) {
  const safePriority = ["High","Medium","Low"].includes(priority) ? priority : "Low";
  return `<span class="priority ${safePriority.toLowerCase()}">${escapeHTML(safePriority)}</span>`;
}

/* render(): repaint from the last data AND ask the server for fresh data (the "Refresh" button uses this). */
function render() { draw(); refresh(); }

function draw() {
  // The server's priority queue (a binary heap) decides the order; the browser only displays it.
  const byId = new Map(emergencies.map(item => [item.id, item]));
  const sorted = queueOrder.map(id => byId.get(id)).filter(item => item && item.status === "Waiting");

  document.getElementById("queueRows").innerHTML = sorted.map((item,index) => `
    <tr>
      <td>${index+1}</td>
      <td><b>${escapeHTML(item.type)}</b></td>
      <td>${badge(item.priority)}</td>
      <td>${escapeHTML(item.location)}</td>
      <td>${escapeHTML(timeAgo(item.createdAt))}</td>
      <td><span class="status">${escapeHTML(item.status)}</span></td>
      <td><button class="btn red" onclick="dispatchNext(${Number(item.id)})">Dispatch</button></td>
    </tr>`).join("") || '<tr><td colspan="7" class="empty">No waiting emergencies. Add a new request to continue.</td></tr>';

  document.getElementById("recentRows").innerHTML = emergencies
    .slice()
    .sort((a,b) => b.id - a.id)
    .slice(0,5)
    .map(item => `<tr>
      <td><b>${escapeHTML(item.type)}</b></td>
      <td>${badge(item.priority)}</td>
      <td>${escapeHTML(item.location)}</td>
      <td>${escapeHTML(timeAgo(item.createdAt))}</td>
      <td><span class="status">${escapeHTML(item.status)}</span></td>
    </tr>`).join("");

  document.getElementById("historyRows").innerHTML = history.map(item => `
    <tr>
      <td><b>${escapeHTML(item.type)}</b></td>
      <td>${badge(item.priority)}</td>
      <td>${escapeHTML(item.ambulance)}</td>
      <td>${escapeHTML(item.location)}</td>
      <td>${escapeHTML(clockTime(item.time))}</td>
      <td><span class="status">${escapeHTML(item.status)}</span></td>
      <td>${item.status === "In Progress" ? `<button class="btn blue" onclick="completeTrip(${Number(item.dispatchId)})">Complete</button>` : "—"}</td>
    </tr>`).join("") || '<tr><td colspan="7" class="empty">No dispatch records yet. Dispatch a request to see history here.</td></tr>';

  const waiting = emergencies.filter(item => item.status === "Waiting");
  const active = emergencies.filter(item => item.status === "Waiting" || item.status === "Dispatched").length;
  const available = fleet.filter(ambulance => ambulance.status === "Available").length;
  document.getElementById("qcount").textContent = waiting.length;
  document.getElementById("hcount").textContent = waiting.filter(item => item.priority === "High").length;
  document.getElementById("activeCount").textContent = active;
  document.getElementById("availableCount").textContent = available;
  document.getElementById("fleetAvailable").textContent = available;
  document.getElementById("fleetDuty").textContent = fleet.length - available;
  document.getElementById("dispatchCount").textContent = dispatchedCount;
  // Fleet size is no longer fixed at 8: keep the two "total" labels in step with the database.
  const totalBox = document.querySelector(".fleet .fleetbox strong"); if (totalBox) totalBox.textContent = fleet.length;
  const availableSmall = document.getElementById("availableCount").parentElement.querySelector("small");
  if (availableSmall) availableSmall.textContent = "Out of " + fleet.length + " total";

  document.getElementById("ambulanceList").innerHTML = fleet.map(ambulance => {
    const safeId = escapeHTML(ambulance.id);
    const safeLocation = escapeHTML(ambulance.location);
    const safeStatus = escapeHTML(ambulance.status);
    const safeType = escapeHTML(ambulance.type || "");
    let action = "";
    if (ambulance.status === "Available") {
      action = `<button class="btn blue" onclick="dispatchNext(null,'${safeId}')">Assign</button>`;
    } else if (ambulance.status === "On Duty") {
      action = '<span class="status">On duty</span>';
    } else if (ambulance.status === "On route") {
      action = `<button class="btn light" onclick="completeByAmb('${safeId}')">Complete</button>`;
    }
    const statusStyle = ambulance.status !== "Available" ? 'background:#fff0d1;color:#a76a00' : '';
    return `<div class="ambrow">
      <div class="ambico">🚑</div>
      <div class="ambtext"><b>Ambulance ${safeId}</b><small>📍 ${safeLocation}${safeType ? " · " + safeType : ""}</small></div>
      <span class="tag" style="${statusStyle}">${safeStatus}</span>${action}
    </div>`;
  }).join("");
}

/* ---------------------------------------------------------------- dispatching */
async function dispatchNext(requestId = null, ambulanceId = null) {
  const body = {};
  if (requestId !== null) body.requestId = Number(requestId);
  if (ambulanceId) body.ambulanceId = ambulanceId;
  let result;
  try { result = await api("/api/dispatch", "POST", body); }
  catch (error) { toast(error.message); await refresh(); return; }
  const em = result.emergency, amb = result.ambulance;
  const hasDistance = result.distanceKm !== null && result.distanceKm !== undefined;
  const how = result.method === "dijkstra" ? "Nearest suitable ambulance (Dijkstra)"
            : result.method === "manual" ? "Chosen by dispatcher" : "First suitable ambulance";
  document.getElementById("dispatchDetails").innerHTML = `
    <div style="font-size:42px;margin:12px">🚑</div>
    <h2>Ambulance ${escapeHTML(amb.id)} Assigned Successfully!</h2>
    <p><b>${escapeHTML(em.type)}</b></p>
    <p>Location: ${escapeHTML(em.location)}</p>
    <p>Priority: ${escapeHTML(em.priority)}</p>
    <p>${escapeHTML(how)}${hasDistance ? " · " + Number(result.distanceKm).toFixed(1) + " km" : ""}${result.route ? "<br>Route: " + escapeHTML(result.route) : ""}</p>
    ${result.warning ? `<p class="sub">${escapeHTML(result.warning)}</p>` : ""}
    <span class="tag">Dispatch simulated successfully</span>
    <p class="sub">No real vehicle has been contacted.</p>`;
  await refresh();
  loadNotifications();
  toast(`Ambulance ${amb.id} assigned to ${em.type}.`);
}
async function completeTrip(dispatchId) {
  try { await api("/api/dispatches/" + Number(dispatchId) + "/complete", "POST"); }
  catch (error) { toast(error.message); await refresh(); return; }
  await refresh();
  loadNotifications();
  toast("Demo trip completed; ambulance is available again.");
}
function completeByAmb(id) {
  const record = history.find(item => item.ambulance === id && item.status === "In Progress");
  if (record) return completeTrip(record.dispatchId);
}
async function resetDemo() {
  if (!confirm("Reset all demo emergency and dispatch records?")) return;
  try { await api("/api/admin/reset", "POST"); }
  catch (error) { toast(error.message); return; }
  const details = document.getElementById("dispatchDetails");
  details.className = "empty";
  details.textContent = "No active assignment. Select an available ambulance to create a dispatch.";
  await refresh();
  loadNotifications();
  toast("Demo data reset.");
}
function toast(message) {
  const element = document.getElementById("toast");
  element.textContent = message;
  element.style.display = "block";
  clearTimeout(window.toastTimer);
  window.toastTimer = setTimeout(() => { element.style.display = "none"; }, 2800);
}

/* ---------------------------------------------------------------- start-up: resume a session if there is one */
(async function boot() {
  if (authToken) {
    try {
      const data = await api("/api/auth/me");
      enterApp(data.user);
      return;
    } catch (error) { /* 401 already cleared the token and showed the sign-in screen; if the server is just unreachable the token is kept */ }
  }
})();
