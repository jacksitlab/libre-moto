/*
  LibreMoto website — minimal i18n (DE/EN) with no build step.
  Static dictionary, data-i18n attributes on elements, localStorage
  persistence + ?lang= override. Translations are trusted static content
  from this file, so innerHTML is safe here.
*/
(function () {
  "use strict";

  var I18N = {
    /* shared */
    "nav-home": { de: "Start", en: "Home" },
    "nav-setup": { de: "Setup", en: "Setup" },
    "nav-hardware": { de: "Hardware", en: "Hardware" },
    "nav-faq": { de: "FAQ", en: "FAQ" },
    "nav-github": { de: "GitHub", en: "GitHub" },
    "skip-content": { de: "Zum Inhalt springen", en: "Skip to content" },
    "footer-project-title": { de: "Projekt", en: "Project" },
    "footer-docs-title": { de: "Dokumentation", en: "Documentation" },
    "footer-docs-readme": { de: "README (Repo)", en: "README (repo)" },
    "footer-docs-protocol": { de: "BLE-Protokoll", en: "BLE protocol" },
    "footer-docs-hardware": { de: "Hardware-Referenz", en: "Hardware reference" },
    "footer-docs-design": { de: "UI-Design", en: "UI design" },
    "footer-legal-title": { de: "Rechtliches", en: "Legal" },
    "footer-license": { de: "Lizenz (GPLv3)", en: "License (GPLv3)" },
    "footer-note": {
      de: "LibreMoto ist freie Software (GPLv3). „Beeline“ und „Beeline Moto II“ sind Marken ihrer jeweiligen Eigentümer; LibreMoto ist ein unabhängiges Projekt und steht in keiner Verbindung zu Beeline.",
      en: "LibreMoto is free software (GPLv3). “Beeline” and “Beeline Moto II” are trademarks of their respective owners; LibreMoto is an independent project and is not affiliated with Beeline."
    },

    /* home */
    "home-title": { de: "LibreMoto — Open-Source-Navi für den Lenker", en: "LibreMoto — open-source handlebar navigation" },
    "hero-badge": { de: "Open Source · GPLv3", en: "Open source · GPLv3" },
    "hero-lead": {
      de: "Ein rundes Touch-Display für den Motorrad-Lenker, das Navigationsanweisungen und eine minimalistische Karte direkt am Griff zeigt — angesteuert per Bluetooth Low Energy von deinem Smartphone. Kein Proprietärsystem, keine Cloud: freie Software auf freier Hardware.",
      en: "A round touch display for your motorcycle handlebar that shows turn instructions and a minimalist map right where you need it — driven over Bluetooth Low Energy by your phone. No proprietary system, no cloud: free software on open hardware."
    },
    "hero-btn-setup": { de: "Setup ansehen", en: "View setup" },
    "hero-btn-github": { de: "Auf GitHub ansehen", en: "View on GitHub" },
    "home-features-title": { de: "Prinzipien", en: "Principles" },
    "feat-distraction-title": { de: "Ablenkungsarm", en: "Low distraction" },
    "feat-distraction-text": {
      de: "Eine Ansicht, die nur das zeigt, was auf der Straße zählt: nächste Anweisung, Entfernung, Tempo­limit. Keine Benachrichtigungen, kein Scrollen.",
      en: "One view that shows only what matters on the road: next instruction, distance, speed limit. No notifications, no scrolling."
    },
    "feat-map-title": { de: "Minimale Karte", en: "Minimalist map" },
    "feat-map-text": {
      de: "Eine klare Vektor-Karte — Route weiß, Straßen schwarz mit weißem Rand — rotiert mit deiner Fahrtrichtung. Keine Kacheln, kein Rendern auf dem Display.",
      en: "A clean vector map — route in white, roads as black lines with white edges — rotating with your heading. No tiles, no rendering on the display."
    },
    "feat-opensource-title": { de: "Frei & offen", en: "Free & open" },
    "feat-opensource-text": {
      de: "Firmware und App stehen unter GPLv3. Kein geschlossenes Produkt darf aus diesem Code gebaut werden — bewusste Entscheidung.",
      en: "Firmware and app are GPLv3. No closed product may be built from this code — a deliberate decision."
    },
    "feat-privacy-title": { de: "Deine Daten bleiben deine", en: "Your data stays yours" },
    "feat-privacy-text": {
      de: "Das Display kennt nur die Route, die dein Telefon ohnehin kennt. Kein Konto, keine Telemetrie, kein Standort-Upload.",
      en: "The display only knows the route your phone already knows. No account, no telemetry, no location upload."
    },
    "feat-brain-title": { de: "Telefon ist das Gehirn", en: "The phone is the brain" },
    "feat-brain-text": {
      de: "Das Telefon berechnet Route und Kartenprojektion und sendet fertige Vektordaten — das Display rendert nur. Dadurch: minimales RAM, stabile Übertragung.",
      en: "The phone computes the route and map projection and sends finished vector data — the display just renders. Result: minimal RAM, stable transfers."
    },
    "feat-gloves-title": { de: "Handschuh-tauglich", en: "Glove-friendly" },
    "feat-gloves-text": {
      de: "Große Ziele, klare Zustände, keine Mini-Buttons. Bedienung per Tippen, Halten und Wischen — mit Handschuhen.",
      en: "Big targets, clear states, no tiny buttons. Operated by tap, hold and swipe — with gloves on."
    },
    "home-how-title": { de: "Wie es zusammensteckt", en: "How it fits together" },
    "home-how-text": {
      de: "Drei Teile, ein Datenfluss: OsmAnd navigiert, die LibreMoto-App projiziert und sendet, das Display rendert.",
      en: "Three parts, one data flow: OsmAnd navigates, the LibreMoto app projects and sends, the display renders."
    },
    "flow-phone-title": { de: "Smartphone", en: "Smartphone" },
    "flow-phone-text": {
      de: "OsmAnd berechnet die Route. Die LibreMoto-App liest sie über die AIDL-API und lädt regionale Vektorkarten (.lmm) aus OpenStreetMap-Daten.",
      en: "OsmAnd computes the route. The LibreMoto app reads it via the AIDL API and loads regional vector maps (.lmm) built from OpenStreetMap data."
    },
    "flow-ble-title": { de: "BLE-Verbindung", en: "BLE link" },
    "flow-ble-text": {
      de: "Bluetooth Low Energy mit eigenem GATT-Protokoll: Navigationsdaten als JSON, Kartenframes als kompaktes Binärformat.",
      en: "Bluetooth Low Energy with a custom GATT protocol: navigation data as JSON, map frames as a compact binary format."
    },
    "flow-display-title": { de: "Display", en: "Display" },
    "flow-display-text": {
      de: "ESP32-S3 mit rundem 2,1″-Touchdisplay rendert Karte und Anweisung — ohne eigenes Routing, ohne Kachel-Download.",
      en: "An ESP32-S3 with a round 2.1″ touch display renders map and instruction — no routing of its own, no tile downloads."
    },
    "home-status-title": { de: "Projektstatus", en: "Project status" },
    "home-status-text": {
      de: "LibreMoto ist aktiv in Entwicklung. Firmware und App sind funktionsfähig und auf echter Hardware im Einsatz — der erste Release (v0.1.0) steht noch aus.",
      en: "LibreMoto is under active development. Firmware and app are functional and running on real hardware — the first release (v0.1.0) is still pending."
    },
    "home-status-btn": { de: "Roadmap im Repo", en: "Roadmap in the repo" },

    /* setup */
    "setup-title": { de: "Setup — LibreMoto", en: "Setup — LibreMoto" },
    "setup-heading": { de: "Setup", en: "Setup" },
    "setup-lead": {
      de: "Von null zu fahrbereit in vier Schritten. Alles ist Open Source — es gibt kein fertiges Image, du baust selbst.",
      en: "From zero to ride-ready in four steps. Everything is open source — there is no ready-made image, you build it yourself."
    },
    "setup-prereq-title": { de: "Was du brauchst", en: "What you need" },
    "setup-prereq-item1": { de: "Ein CrowPanel 2.1″ (ESP32-S3, 480×480 rund) oder einen kompatiblen Board-Bausatz", en: "A CrowPanel 2.1″ (ESP32-S3, 480×480 round) or a compatible board kit" },
    "setup-prereq-item2": { de: "Ein Android-Smartphone (Android 10+) mit OsmAnd", en: "An Android phone (Android 10+) with OsmAnd" },
    "setup-prereq-item3": { de: "Einen Rechner mit ESP-IDF (v5.3 oder v6.x) und Android Studio", en: "A computer with ESP-IDF (v5.3 or v6.x) and Android Studio" },
    "setup-prereq-item4": { de: "Ein USB-C-Kabel für Flashen und Strom am Lenker", en: "A USB-C cable for flashing and for power on the handlebar" },
    "setup-step1-title": { de: "1 · Firmware flashen", en: "1 · Flash the firmware" },
    "setup-step1-text": {
      de: "Klone das Repository und baue die Firmware mit ESP-IDF. Das Board meldet sich danach als „LibreMoto“ per Bluetooth.",
      en: "Clone the repository and build the firmware with ESP-IDF. The board then advertises itself as “LibreMoto” over Bluetooth."
    },
    "setup-step2-title": { de: "2 · App installieren", en: "2 · Install the app" },
    "setup-step2-text": {
      de: "Die Android-App baut du aus dem selben Repository (Android Studio oder Gradle). Sie verbindet sich automatisch mit dem Display.",
      en: "Build the Android app from the same repository (Android Studio or Gradle). It connects to the display automatically."
    },
    "setup-step3-title": { de: "3 · OsmAnd einrichten", en: "3 · Set up OsmAnd" },
    "setup-step3-text": {
      de: "Für die Live-Karte braucht die App Route und Straßendaten aus OsmAnd. Dafür gibt es einen gepatchten OsmAnd-Build (AIDL-Erweiterungen) — das Build-Skript liegt im Repository.",
      en: "For the live map the app needs route and road data from OsmAnd. That requires a patched OsmAnd build (AIDL extensions) — the build script is in the repository."
    },
    "setup-step4-title": { de: "4 · Karten laden", en: "4 · Load maps" },
    "setup-step4-text": {
      de: "In der App lädst du Regionen aus dem Geofabrik-Index (z. B. Brandenburg). Die Region wird auf dem Gerät in das .lmm-Format umgewandelt — fertig.",
      en: "In the app you download regions from the Geofabrik index (e.g. Brandenburg). The region is converted to the .lmm format on-device — done."
    },
    "setup-step4-note": {
      de: "Kein Befehl — Regionen werden direkt in der App ausgewählt und heruntergeladen.",
      en: "No command — regions are selected and downloaded directly in the app."
    },
    "setup-more-title": { de: "Details", en: "Details" },
    "setup-more-text": {
      de: "Komplette Anleitung mit allen Befehlen: README im Repository.",
      en: "Complete guide with all commands: the README in the repository."
    },
    "setup-more-btn": { de: "Zum README", en: "Go to the README" },

    /* hardware */
    "hw-title": { de: "Hardware — LibreMoto", en: "Hardware — LibreMoto" },
    "hw-heading": { de: "Hardware", en: "Hardware" },
    "hw-lead": {
      de: "Bewusst einfache, offene Hardware: ein ESP32-S3, ein rundes Touchdisplay, sonst nichts. Kein geschlossenes Modul, kein Vendor-Lock-in.",
      en: "Deliberately simple, open hardware: an ESP32-S3, a round touch display, nothing else. No closed module, no vendor lock-in."
    },
    "hw-spec-title": { de: "Technische Daten (CrowPanel 2.1″)", en: "Specs (CrowPanel 2.1″)" },
    "hw-spec-mcu": { de: "Mikrocontroller", en: "Microcontroller" },
    "hw-spec-mcu-v": { de: "ESP32-S3R8 — dual-core Xtensa LX7, 240 MHz, 512 KB SRAM, 8 MB PSRAM, 16 MB Flash", en: "ESP32-S3R8 — dual-core Xtensa LX7, 240 MHz, 512 KB SRAM, 8 MB PSRAM, 16 MB flash" },
    "hw-spec-display": { de: "Display", en: "Display" },
    "hw-spec-display-v": { de: "2,1″ IPS, 480×480 px, rund, RGB-parallel, ST7701S-Treiber, ~460 Nits", en: "2.1″ IPS, 480×480 px, round, RGB parallel, ST7701S driver, ~460 nits" },
    "hw-spec-touch": { de: "Eingabe", en: "Input" },
    "hw-spec-touch-v": { de: "CST816-Touch über I²C — Tippen, Halten, Wischen (kein Drehgeber)", en: "CST816 touch over I²C — tap, hold, swipe (no rotary knob)" },
    "hw-spec-io": { de: "I/O-Expander", en: "I/O expander" },
    "hw-spec-io-v": { de: "PCF8574 (I²C) für Reset-, IRQ- und LCD-Power-Signale", en: "PCF8574 (I²C) for reset, IRQ and LCD power signals" },
    "hw-spec-power": { de: "Stromversorgung", en: "Power" },
    "hw-spec-power-v": { de: "5 V über USB-C", en: "5 V via USB-C" },
    "hw-spec-link": { de: "Anbindung", en: "Connectivity" },
    "hw-spec-link-v": { de: "Bluetooth Low Energy (GATT-Server)", en: "Bluetooth Low Energy (GATT server)" },
    "hw-spec-extra": { de: "Besonderheiten", en: "Features" },
    "hw-spec-size": { de: "Abmessungen", en: "Dimensions" },
    "hw-boards-title": { de: "Boards", en: "Boards" },
    "hw-boards-text": {
      de: "Die Firmware hat eine Hardware-Abstraktionsschicht (HAL) — pro Board ein Treibersatz, ausgewählt beim Build. Aktuell unterstützt:",
      en: "The firmware has a hardware abstraction layer (HAL) — one driver set per board, selected at build time. Currently supported:"
    },
    "hw-board-crowpanel": {
      de: "Elecrow CrowPanel 2.1″ — ESP32-S3, ST7701S 480×480, CST816-Touch. Das Referenz-Board des Projekts.",
      en: "Elecrow CrowPanel 2.1″ — ESP32-S3, ST7701S 480×480, CST816 touch. The project's reference board."
    },
    "hw-board-waveshare": {
      de: "Waveshare ESP32-S3-Touch-AMOLED-1.75C — 466×466 AMOLED über QSPI. Board-Support in Arbeit.",
      en: "Waveshare ESP32-S3-Touch-AMOLED-1.75C — 466×466 AMOLED via QSPI. Board support in progress."
    },
    "hw-boards-own": {
      de: "Eigenes Board? Die HAL-Schnittstelle ist klein: Display, Touch, Power — der Rest (Protokoll, Renderer, UI) wird geteilt.",
      en: "Your own board? The HAL interface is small: display, touch, power — everything else (protocol, renderer, UI) is shared."
    },
    "hw-mount-title": { de: "Montage & Strom", en: "Mounting & power" },
    "hw-mount-text": {
      de: "Das Board läuft über USB-C mit 5 V — am Motorrad direkt von einem USB-Ladegerät am Akkerausgang. Einbinden in einen Lenkerhalter ist reine Mechanik; es gibt kein proprietäres Gehäuse.",
      en: "The board runs on 5 V via USB-C — on a bike straight from a USB charger on the battery. Fitting it into a handlebar mount is pure mechanics; there is no proprietary case."
    },

    /* faq */
    "faq-title": { de: "FAQ — LibreMoto", en: "FAQ — LibreMoto" },
    "faq-heading": { de: "FAQ", en: "FAQ" },
    "faq-lead": {
      de: "Häufige Fragen zu LibreMoto. Wenn deine Frage hier nicht auftaucht: Issue im Repository stellen.",
      en: "Frequently asked questions about LibreMoto. If your question isn't here: open an issue on the repository."
    },
    "faq-q1": { de: "Was ist LibreMoto genau?", en: "What exactly is LibreMoto?" },
    "faq-a1": {
      de: "Ein open-source Navigationssystem für Motorräder: ein rundes Touchdisplay am Lenker (ESP32-S3), das per Bluetooth Low Energy Navigationsdaten und eine minimalistische Karte von deinem Android-Smartphone bekommt. OsmAnd liefert die Navigation, die LibreMoto-App übersetzt sie fürs Display.",
      en: "An open-source navigation system for motorcycles: a round touch display on the handlebar (ESP32-S3) that receives navigation data and a minimalist map from your Android phone over Bluetooth Low Energy. OsmAnd does the navigation, the LibreMoto app translates it for the display."
    },
    "faq-q2": { de: "Kann ich es einfach kaufen?", en: "Can I just buy it?" },
    "faq-a2": {
      de: "Nein — es gibt kein fertiges Produkt. LibreMoto ist ein Open-Source-Projekt: Du besorgst die Hardware (CrowPanel 2.1″ oder kompatibel), flasht die Firmware und baust die App selbst. Genau das ist der Punkt.",
      en: "No — there is no finished product. LibreMoto is an open-source project: you get the hardware (CrowPanel 2.1″ or compatible), flash the firmware and build the app yourself. That is exactly the point."
    },
    "faq-q3": { de: "Welches Smartphone brauche ich?", en: "Which phone do I need?" },
    "faq-a3": {
      de: "Ein Android-Gerät mit Android 10 oder neuer. Die App läuft mit OsmAnd — der Standard-Build für Anweisungen, ein gepatchter Build (AIDL-Erweiterungen für Routengeometrie und Straßendaten, Skript im Repository) für die Karte. iPhone wird nicht unterstützt.",
      en: "An Android device with Android 10 or newer. The app works with OsmAnd — the stock build for instructions, a patched build (AIDL extensions for route geometry and road data, script in the repository) for the map. iPhone is not supported."
    },
    "faq-q4": { de: "Wie funktioniert die Karte ohne Internetverbindung?", en: "How does the map work without an internet connection?" },
    "faq-a4": {
      de: "Die App lädt Regionen aus dem Geofabrik-Index (OpenStreetMap-Daten) einmalig herunter und wandelt sie auf dem Gerät in ein kompaktes Vektorformat (.lmm) um. Ab dann arbeitet alles offline: Route kommt von OsmAnd, Straßen aus der lokalen Kartendatei.",
      en: "The app downloads regions from the Geofabrik index (OpenStreetMap data) once and converts them on-device into a compact vector format (.lmm). From then on everything works offline: the route comes from OsmAnd, the streets from the local map file."
    },
    "faq-q5": { de: "Kann ich ein anderes Display verwenden?", en: "Can I use a different display?" },
    "faq-a5": {
      de: "Ja, mit Eigenaufwand: Die Firmware hat eine Hardware-Abstraktionsschicht — pro Board ein Treibersatz (Display-Init, Touch, Power). Aktuell sind der CrowPanel 2.1″ (Referenz) und das Waveshare AMOLED 1.75C (in Arbeit) angebunden. Ein neues Board heißt: ein neues HAL-Verzeichnis, nicht ein neuer Fork.",
      en: "Yes, with some effort: the firmware has a hardware abstraction layer — one driver set per board (display init, touch, power). Currently the CrowPanel 2.1″ (reference) and the Waveshare AMOLED 1.75C (in progress) are supported. A new board means: a new HAL directory, not a new fork."
    },
    "faq-q6": { de: "Was passiert, wenn die Bluetooth-Verbindung abreißt?", en: "What happens when the Bluetooth connection drops?" },
    "faq-a6": {
      de: "Das Display zeigt den Verbindungsstatus (Status-LED auf dem Home-Screen) und verbindet sich automatisch wieder, sobald das Telefon in Reichweite ist — die App wie die Firmware bauen die Verbindung selbstständig neu auf. Die letzte Karte bleibt stehen, bis neue Daten kommen.",
      en: "The display shows the connection state (status LED on the home screen) and reconnects automatically once the phone is back in range — both the app and the firmware rebuild the link on their own. The last map stays on screen until fresh data arrives."
    },
    "faq-q7": { de: "Wackelt die Karte bei Nässe oder mit Handschuhen?", en: "Does the map jitter in the rain or with gloves?" },
    "faq-a7": {
      de: "Die Karte rotiert mit einer geglätteten Kompassrichtung (nicht jedem GPS-Paket einzeln hinterher) — das verhindert das übliche Zittern pro Frame. Bedienkonzept ist bewusst grob: Tippen, Halten, Wischen; keine kleinen Schalter.",
      en: "The map rotates with a smoothed heading (not chasing every GPS packet individually) — that prevents the usual per-frame jitter. The interaction concept is deliberately coarse: tap, hold, swipe; no small toggles."
    },
    "faq-q8": { de: "Unter welcher Lizenz steht das?", en: "What license is it under?" },
    "faq-a8": {
      de: "GPLv3 — Firmware und App, absichtlich Copyleft: Aus diesem Code darf kein geschlossenes Produkt gebaut werden. „Beeline“ ist eine Marke ihrer Eigentümer; LibreMoto ist unabhängig und nicht affiliiert.",
      en: "GPLv3 — firmware and app, deliberately copyleft: no closed product may be built from this code. “Beeline” is a trademark of its owners; LibreMoto is independent and not affiliated."
    },
    "faq-q9": { de: "Wie kann ich beitragen?", en: "How can I contribute?" },
    "faq-a9": {
      de: "Issues und Pull Requests im Repository sind willkommen — besonders rund um neue Boards (HAL-Treiber), Kartenfilter und UX. Der Status der Meilensteine steht im README.",
      en: "Issues and pull requests on the repository are welcome — especially around new boards (HAL drivers), map filters and UX. Milestone status is tracked in the README."
    }
  };

  function setLang(lang) {
    if (lang !== "de" && lang !== "en") lang = "de";
    document.documentElement.setAttribute("lang", lang);
    document.querySelectorAll("[data-i18n]").forEach(function (el) {
      var key = el.getAttribute("data-i18n");
      if (I18N[key] && I18N[key][lang] !== undefined) el.innerHTML = I18N[key][lang];
    });
    document.querySelectorAll("[data-lang]").forEach(function (btn) {
      btn.classList.toggle("active", btn.getAttribute("data-lang") === lang);
    });
    try { localStorage.setItem("lm-lang", lang); } catch (e) { /* private mode */ }
  }

  function initialLang() {
    var param = new URLSearchParams(window.location.search).get("lang");
    if (param === "de" || param === "en") return param;
    try {
      var saved = localStorage.getItem("lm-lang");
      if (saved === "de" || saved === "en") return saved;
    } catch (e) { /* private mode */ }
    var nav = (navigator.language || "de").toLowerCase();
    return nav.indexOf("de") === 0 ? "de" : "en";
  }

  document.addEventListener("DOMContentLoaded", function () {
    document.querySelectorAll("[data-lang]").forEach(function (btn) {
      btn.addEventListener("click", function () { setLang(btn.getAttribute("data-lang")); });
    });
    setLang(initialLang());
  });
})();
