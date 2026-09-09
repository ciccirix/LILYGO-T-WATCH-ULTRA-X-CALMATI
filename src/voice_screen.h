#pragma once

// Schermata "Assistente UDDA": registra la voce dal microfono, la manda a UDDA
// (Pi, servizio :8082) via WiFi, e mostra a schermo la trascrizione + la
// risposta dell'assistente locale. Tutto offline (Vosk + ollama sul Pi).
void voice_screen_show();
