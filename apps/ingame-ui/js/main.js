// GAGE-MP in-game UI.
//
// Thin consumer of the `mp.*` bridge the engine (gage_mp.dll) exposes to the
// CEF/V8 layer. Same surface as the original: mp.invoke / mp.events, with
// chat:push / chat:clear / chat:activate / chat:show events. T focuses chat,
// Enter sends, `/` prefixes a command.

(function () {
  "use strict";

  var chatEl   = document.getElementById("chat");
  var inputEl  = document.getElementById("chat-input");
  var statusEl = document.getElementById("status");

  function el(tag, cls, text) {
    var n = document.createElement(tag);
    if (cls) n.className = cls;
    if (text != null) n.textContent = text;
    return n;
  }

  function pushLine(name, text, cls) {
    var line = el("div", "line" + (cls ? " " + cls : ""));
    if (name) line.appendChild(el("span", "who", name));
    line.appendChild(document.createTextNode(" " + text));
    chatEl.appendChild(line);
    chatEl.scrollTop = chatEl.scrollHeight;
  }

  // ---- engine -> UI events ------------------------------------------------
  if (window.mp && mp.events && mp.events.add) {
    mp.events.add("chat:push", function (data) {
      var p = typeof data === "string" ? data : (data && data.text) || "";
      pushLine(data && data.name ? data.name : "", p, "in");
    });
    mp.events.add("chat:clear", function () {
      chatEl.innerHTML = "";
    });
    mp.events.add("chat:show", function () {
      chatEl.classList.remove("hidden");
    });
    mp.events.add("chat:hide", function () {
      chatEl.classList.add("hidden");
    });
    mp.events.add("status", function (msg) {
      statusEl.textContent = msg;
    });
  }

  function send() {
    var v = inputEl.value.trim();
    if (!v) return;
    if (v.charAt(0) === "/")
      mp.invoke("command", v);
    else
      mp.invoke("chatMessage", v);
    inputEl.value = "";
  }

  inputEl.addEventListener("keydown", function (e) {
    if (e.key === "Enter") { send(); }
  });

  // ---- keyboard: T toggles chat focus, Esc blurs --------------------------
  window.addEventListener("keydown", function (e) {
    if (e.key.toLowerCase() === "t" && document.activeElement !== inputEl) {
      e.preventDefault();
      inputEl.focus();
      if (mp.invoke) mp.invoke("focus");
    } else if (e.key === "Escape" && document.activeElement === inputEl) {
      inputEl.blur();
    }
  });

  inputEl.addEventListener("focus",  function () { if (mp.invoke) mp.invoke("setTypingInChatState", "true"); });
  inputEl.addEventListener("blur",   function () { if (mp.invoke) mp.invoke("setTypingInChatState", "false"); });

  statusEl.textContent = "ready — engine " + ((mp && mp.version) || "1.0.0");
})();
