/*
 * SwitchAirCast - user interface text in English and Spanish
 *
 * Spanish follows the console's own terms (Configuración de la consola) and iOS's
 * (Centro de control, Duplicar pantalla). Latin-American Spanish only differs where
 * the regions do: "video" for "vídeo" and "presiona" for "pulsa".
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include "i18n.h"

#include <string.h>
#include <strings.h>

static const char *const en[S_COUNT] = {
    [S_APP_SUBTITLE] = "AirPlay receiver",
    [S_EXIT] = "Exit",
    [S_SETTINGS] = "Settings",
    [S_SCREEN_OFF] = "Screen off",
    [S_STATS] = "Stats",
    [S_HIDE_STATS] = "Hide stats",
    [S_DISCONNECT] = "Disconnect",
    [S_TRY_AGAIN] = "Try again",
    [S_ROTATE] = "Rotate",
    [S_STARTING] = "Starting\xE2\x80\xA6",
    [S_NO_NETWORK_TITLE] = "Not connected to a network",
    [S_NO_NETWORK_LINE1] = "Connect this Switch to Wi-Fi in System Settings \xE2\x86\x92 Internet.",
    [S_NO_NETWORK_LINE2] = "SwitchAirCast will start automatically once it is connected.",
    [S_START_ERROR_TITLE] = "Could not start the AirPlay receiver",
    [S_ERR_DISCOVERY] = "Could not set up network discovery (error %d).",
    [S_ERR_SERVER] = "Could not initialize the AirPlay server.",
    [S_ERR_PORT] = "Could not open a network port for AirPlay.",
    [S_ERR_ADVERTISE] = "Could not advertise the receiver on the network (error %d).",
    [S_READY_WAITING] = "Ready \xE2\x80\x94 waiting for a device",
    [S_CONNECTED_TO] = "Connected to %s",
    [S_A_DEVICE] = "a device",
    [S_YOUR_DEVICE] = "your device",
    [S_IP_ADDRESS] = "IP address %s",
    [S_STEP_WIFI] = "Connect your iPhone, iPad or Mac to the same Wi-Fi network as this Switch.",
    [S_STEP_CONTROL_CENTER] = "Open Control Center and tap Screen Mirroring, or tap AirPlay in YouTube or a music app.",
    [S_STEP_CHOOSE] = "Choose \xE2\x80\x9C%s\xE2\x80\x9D from the list.",
    [S_PLAYING_FROM] = "Playing from %s",
    [S_PLAYING_AUDIO] = "Playing audio",
    [S_STARTING_MIRROR_FROM] = "Starting screen mirroring from %s",
    [S_MIRRORING_FROM] = "Mirroring from %s",
    [S_LOADING_VIDEO] = "Loading video",
    [S_LOADING_VIDEO_FROM] = "Loading video from %s",
    [S_PLAYING_VIDEO] = "Playing AirPlay video",
    [S_PLAYING_VIDEO_FROM] = "Playing video from %s",
    [S_VIDEO_FAILED_TITLE] = "This video can\xE2\x80\x99t be played",
    [S_VIDEO_FAILED_LINE1] = "The app sent a stream SwitchAirCast cannot open (protected or unsupported).",
    [S_VIDEO_FAILED_LINE2] = "You can still show it with Screen Mirroring.",
    [S_PLAY] = "Play",
    [S_PAUSE] = "Pause",
    [S_STOP] = "Stop",
    [S_SECONDS_10] = "10 s",
    [S_MINUTE_1] = "1 min",
    [S_HARDWARE] = "Hardware",
    [S_SOFTWARE] = "Software",
    [S_MS_PER_FRAME] = "%.1f ms/frame",
    [S_FRAMES_SKIPPED] = "%lu frames  \xC2\xB7  %lu skipped",
    [S_AUDIO_ON] = "audio on",
    [S_AUDIO_OFF] = "no audio",
    [S_SET_LANGUAGE] = "Language",
    [S_SET_THEME] = "Theme",
    [S_SET_SCREEN_OFF] = "Turn the screen off after",
    [S_SET_ROTATION] = "Vertical mode",
    [S_VAL_AUTOMATIC] = "Automatic",
    [S_VAL_ENGLISH] = "English",
    [S_VAL_SPANISH] = "Espa\xC3\xB1ol",
    [S_VAL_THEME_DARK] = "Dark",
    [S_VAL_THEME_BLACK] = "OLED black",
    [S_VAL_NEVER] = "Never",
    [S_VAL_MINUTES] = "%d min",
    [S_VAL_OFF] = "Off",
    [S_VAL_ROTATION_AUTO] = "Automatic (sensor)",
    [S_VAL_ROTATE_LEFT] = "Rotate left",
    [S_VAL_ROTATE_RIGHT] = "Rotate right",
    [S_CHANGE] = "Change",
    [S_CLOSE] = "Close",
    [S_SETTINGS_NOTE] = "The screen turns off only while waiting or playing music, never during a video.",
    [S_SCREEN_OFF_TOAST] = "Screen off \xE2\x80\x94 press any button to turn it on",
    [S_ROTATION_TOAST_OFF] = "Vertical mode off",
    [S_ROTATION_TOAST_LEFT] = "Vertical mode: rotated left",
    [S_ROTATION_TOAST_RIGHT] = "Vertical mode: rotated right",
};

static const char *const es[S_COUNT] = {
    [S_APP_SUBTITLE] = "Receptor AirPlay",
    [S_EXIT] = "Salir",
    [S_SETTINGS] = "Ajustes",
    [S_SCREEN_OFF] = "Apagar pantalla",
    [S_STATS] = "Estad\xC3\xADsticas",
    [S_HIDE_STATS] = "Ocultar estad\xC3\xADsticas",
    [S_DISCONNECT] = "Desconectar",
    [S_TRY_AGAIN] = "Reintentar",
    [S_ROTATE] = "Girar",
    [S_STARTING] = "Iniciando\xE2\x80\xA6",
    [S_NO_NETWORK_TITLE] = "Sin conexi\xC3\xB3n a una red",
    [S_NO_NETWORK_LINE1] = "Conecta esta Switch a una red Wi-Fi en Configuraci\xC3\xB3n de la consola \xE2\x86\x92 Internet.",
    [S_NO_NETWORK_LINE2] = "SwitchAirCast se iniciar\xC3\xA1 autom\xC3\xA1ticamente en cuanto se conecte.",
    [S_START_ERROR_TITLE] = "No se pudo iniciar el receptor AirPlay",
    [S_ERR_DISCOVERY] = "No se pudo configurar la detecci\xC3\xB3n en la red (error %d).",
    [S_ERR_SERVER] = "No se pudo iniciar el servidor AirPlay.",
    [S_ERR_PORT] = "No se pudo abrir un puerto de red para AirPlay.",
    [S_ERR_ADVERTISE] = "No se pudo anunciar el receptor en la red (error %d).",
    [S_READY_WAITING] = "Listo \xE2\x80\x94 esperando un dispositivo",
    [S_CONNECTED_TO] = "Conectado a %s",
    [S_A_DEVICE] = "un dispositivo",
    [S_YOUR_DEVICE] = "tu dispositivo",
    [S_IP_ADDRESS] = "Direcci\xC3\xB3n IP %s",
    [S_STEP_WIFI] = "Conecta tu iPhone, iPad o Mac a la misma red Wi-Fi que esta Switch.",
    [S_STEP_CONTROL_CENTER] = "Abre el Centro de control y toca Duplicar pantalla, o AirPlay en YouTube o M\xC3\xBAsica.",
    [S_STEP_CHOOSE] = "Elige \xE2\x80\x9C%s\xE2\x80\x9D en la lista.",
    [S_PLAYING_FROM] = "Reproduciendo desde %s",
    [S_PLAYING_AUDIO] = "Reproduciendo audio",
    [S_STARTING_MIRROR_FROM] = "Iniciando la duplicaci\xC3\xB3n de pantalla de %s",
    [S_MIRRORING_FROM] = "Duplicando la pantalla de %s",
    [S_LOADING_VIDEO] = "Cargando el v\xC3\xAD" "deo",
    [S_LOADING_VIDEO_FROM] = "Cargando el v\xC3\xAD" "deo de %s",
    [S_PLAYING_VIDEO] = "Reproduciendo v\xC3\xAD" "deo AirPlay",
    [S_PLAYING_VIDEO_FROM] = "Reproduciendo v\xC3\xAD" "deo de %s",
    [S_VIDEO_FAILED_TITLE] = "Este v\xC3\xAD" "deo no se puede reproducir",
    [S_VIDEO_FAILED_LINE1] = "La app envi\xC3\xB3 una transmisi\xC3\xB3n que SwitchAirCast no puede abrir (protegida o no compatible).",
    [S_VIDEO_FAILED_LINE2] = "Puedes mostrarlo con Duplicar pantalla.",
    [S_PLAY] = "Reproducir",
    [S_PAUSE] = "Pausa",
    [S_STOP] = "Detener",
    [S_SECONDS_10] = "10 s",
    [S_MINUTE_1] = "1 min",
    [S_HARDWARE] = "Hardware",
    [S_SOFTWARE] = "Software",
    [S_MS_PER_FRAME] = "%.1f ms/fotograma",
    [S_FRAMES_SKIPPED] = "%lu fotogramas  \xC2\xB7  %lu omitidos",
    [S_AUDIO_ON] = "con audio",
    [S_AUDIO_OFF] = "sin audio",
    [S_SET_LANGUAGE] = "Idioma",
    [S_SET_THEME] = "Tema",
    [S_SET_SCREEN_OFF] = "Apagar la pantalla tras",
    [S_SET_ROTATION] = "Modo vertical",
    [S_VAL_AUTOMATIC] = "Autom\xC3\xA1tico",
    [S_VAL_ENGLISH] = "English",
    [S_VAL_SPANISH] = "Espa\xC3\xB1ol",
    [S_VAL_THEME_DARK] = "Oscuro",
    [S_VAL_THEME_BLACK] = "Negro OLED",
    [S_VAL_NEVER] = "Nunca",
    [S_VAL_MINUTES] = "%d min",
    [S_VAL_OFF] = "Desactivado",
    [S_VAL_ROTATION_AUTO] = "Autom\xC3\xA1tico (sensor)",
    [S_VAL_ROTATE_LEFT] = "Girar a la izquierda",
    [S_VAL_ROTATE_RIGHT] = "Girar a la derecha",
    [S_CHANGE] = "Cambiar",
    [S_CLOSE] = "Cerrar",
    [S_SETTINGS_NOTE] = "La pantalla solo se apaga mientras espera o reproduce m\xC3\xBAsica, nunca durante un v\xC3\xAD" "deo.",
    [S_SCREEN_OFF_TOAST] = "Pantalla apagada: pulsa cualquier bot\xC3\xB3n para encenderla",
    [S_ROTATION_TOAST_OFF] = "Modo vertical desactivado",
    [S_ROTATION_TOAST_LEFT] = "Modo vertical: girado a la izquierda",
    [S_ROTATION_TOAST_RIGHT] = "Modo vertical: girado a la derecha",
};

/* Latin-American Spanish: only the strings that differ from Spain's */
static const char *const es_419[S_COUNT] = {
    [S_LOADING_VIDEO] = "Cargando el video",
    [S_LOADING_VIDEO_FROM] = "Cargando el video de %s",
    [S_PLAYING_VIDEO] = "Reproduciendo video AirPlay",
    [S_PLAYING_VIDEO_FROM] = "Reproduciendo video de %s",
    [S_VIDEO_FAILED_TITLE] = "Este video no se puede reproducir",
    [S_SETTINGS_NOTE] = "La pantalla solo se apaga mientras espera o reproduce m\xC3\xBAsica, nunca durante un video.",
    [S_SCREEN_OFF_TOAST] = "Pantalla apagada: presiona cualquier bot\xC3\xB3n para encenderla",
};

static lang_t current = LANG_EN;

void i18n_set_language(lang_t lang) {
    current = lang;
}

lang_t i18n_language(void) {
    return current;
}

lang_t i18n_from_tag(const char *tag) {
    if (!tag || strncasecmp(tag, "es", 2) || (tag[2] && tag[2] != '-' && tag[2] != '_' && tag[2] != ':')) {
        return LANG_EN;
    }
    /* es-419 (and any es-XX other than es-ES) is Latin-American Spanish */
    if ((tag[2] == '-' || tag[2] == '_') && strncasecmp(tag + 3, "ES", 2)) return LANG_ES_419;
    return LANG_ES;
}

const char *tr(str_id_t id) {
    if (id < 0 || id >= S_COUNT) return "";
    const char *s = NULL;
    if (current == LANG_ES_419) s = es_419[id];
    if (!s && (current == LANG_ES || current == LANG_ES_419)) s = es[id];
    if (!s) s = en[id];
    return s ? s : "";
}
