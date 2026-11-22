# Spore Collection GOG

Español | [English](README.en.md) | [Русский](README.ru.md)

Port nativo de Spore (con Aventuras Galácticas) para Android. No es un emulador ni Wine: `SporeApp.exe` se recompila
de x86 a C, se compila para arm64 y corre sobre una capa propia de Win32/D3D9 (SDL2, DXVK).

Este repositorio es un fork de **[VinTeRuS/spore-re](https://github.com/VinTeRuS/spore-re)**, el port original
creado por VinTeRuS. Todo el mérito del port es suyo; aquí solo se agregan algunas mejoras. Si te gusta, puedes
apoyarlo en Patreon: **[patreon.com/VinTeRuS](https://www.patreon.com/cw/VinTeRuS/membership)**.

> **El proyecto está en una etapa muy temprana.** Puede haber errores, cierres inesperados y cambios frecuentes.

## Qué tiene de nuevo este fork

Está al día con **spore-re v0.1.5** (todo lo del original: teclado, pantalla sin bordes, botones de la rueda en el
editor, botón de trucos, vuelo en el espacio, etc.), y además trae:

- **Ajustes y controles en español** cuando el teléfono está en español: panel de ajustes, botones de cada etapa,
  pistas, avisos y la pantalla de archivos del juego. Las palabras largas se achican para caber en los botones.
- **Crédito más discreto**: el texto "Spore Android port by VinTeRuS" de la esquina es más pequeño y transparente.
- **Nombre propio**: se instala como **Spore Collection GOG**, al lado de la app oficial, sin reemplazarla.
- **APK compilado en GitHub**: cada cambio genera un APK nuevo en Releases, sin necesitar los archivos del juego.

## Descargar

Los APK están en la página de **[Releases](https://github.com/freedsny/Spore-Collection-GOG/releases)**. El APK no
trae los archivos del juego: necesitas tu propia copia de Spore (ver abajo).

- `Spore-Collection-GOG-<versión>.apk`: para GPUs con Vulkan 1.3 (la mayoría de teléfonos).
- `Spore-Collection-GOG-<versión>-vk11.apk`: para GPUs viejas cuyo driver solo tiene Vulkan 1.1 (Mali y PowerVR antiguos).

## Instalación

Android 10 o superior, arm64 (teléfonos de 2019 en adelante, aproximadamente).

1. Copia las carpetas del juego `Data`, `DataEP1`, `SporebinEP1` y `bp1content` de tu instalación de Spore a
   `Almacenamiento interno/Spore` en el teléfono (debe quedar `/sdcard/Spore/SporebinEP1/SporeApp.exe`).
2. Instala el APK, ábrelo y permite el "Acceso a todos los archivos".
3. El juego usa el idioma del teléfono; puedes elegir otro en Ajustes (botón redondo → engranaje) → Idioma.

Las partidas guardadas van en `Spore/UserData`, junto a los archivos del juego: esta app y la oficial usan las mismas.
En cada inicio, los registros de la partida anterior se copian a `Spore/logs` (útiles para reportar errores).

## Versión del juego

GOG Spore Collection 3.1.0.22, `SporebinEP1/SporeApp.exe` (sha256 en `re/target.sha256`). Otras versiones no
funcionan.

## Estado

- Funcionan todas las etapas (célula, criatura, tribu, civilización, espacio), los editores, las partidas guardadas,
  el sonido y Aventuras Galácticas.
- Controles táctiles según la etapa (con una pista corta): tocar = clic, mantener = clic derecho, un dedo gira la
  cámara o mueve el mapa, dos dedos giran, pellizcar hace zoom. Joystick en criatura y espacio, botones de
  habilidades, botón **Clic** en el espacio (mantener una herramienta: rayos, abducción, terraformar), botón
  **Trucos** en el menú para la consola de trucos (se escribe con ⌨). Todos los botones se pueden mover
  (Ajustes → Mover botones).
- Editores: Deshacer, Rehacer y ▲ / ▼ para el tamaño de las piezas (toca una pieza y luego ▲ / ▼).
- Ajustes: calidad, resolución, **Pantalla** (Completa / 16:9 / Sin bordes, con barras negras; Sin bordes evita las
  esquinas redondeadas y la cámara), nitidez, límite de FPS 30-120, tamaño y opacidad de los botones, idioma.
- Driver de Vulkan propio: pon un `.so` o un `.zip` de Adreno Tools en `Spore/driver/`.
- Problemas conocidos: las excepciones de C++ del juego no se manejan (algunos perfiles con mods se cierran), y
  "Salir" en el menú de pausa cierra la app (es la salida normal del juego).

## Cómo se compila el APK de este fork

Cada cambio que se sube a `android/` hace que GitHub Actions compile los APK solo (`.github/workflows/fork-apk.yml`)
y los publique en Releases. No necesita los archivos del juego: toma el código Java de este repositorio y las
librerías nativas del último release oficial de VinTeRuS (`android/build-fork-apk.sh`). Por eso, por ahora, las
mejoras de este fork son solo en la parte Java (controles, ajustes, textos).

Para cambiar la parte nativa (el motor) hace falta la compilación completa del port original, con el juego
instalado y el Android NDK: ver [README en inglés](README.en.md#building-rough).

## Qué hay en el repositorio

El código del port: el host Win32/D3D9, la app de Android (Java, controles táctiles, ajustes), los scripts de
compilación, el recompilador (`third_party/x86recomp`) y parches para DXVK. No hay archivos del juego ni código del
juego recompilado: ese código se genera al compilar, a partir de tu propia copia del juego.

## Legal

Necesitas tu propia copia comprada de Spore. Spore es una marca registrada de Electronic Arts; este proyecto no está
afiliado a EA ni a Maxis.
