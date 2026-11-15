# Spore Collection GOG

Español | [English](README.en.md) | [Русский](README.ru.md)

Port nativo de Spore (con Aventuras Galácticas) para Android. No es un emulador ni Wine: `SporeApp.exe` se recompila
de x86 a C, se compila para arm64 y corre sobre una capa propia de Win32/D3D9 (SDL2, DXVK).

Este repositorio es un fork de **[VinTeRuS/spore-re](https://github.com/VinTeRuS/spore-re)**, el port original
creado por VinTeRuS. Todo el mérito del port es suyo; aquí solo se agregan algunas mejoras. Si te gusta, puedes
apoyarlo en Patreon: **[patreon.com/VinTeRuS](https://www.patreon.com/cw/VinTeRuS/membership)**.

> **El proyecto está en una etapa muy temprana.** Puede haber errores, cierres inesperados y cambios frecuentes.

## Qué tiene de nuevo este fork

- **Teclado del teléfono dentro del juego**: el botón ⌨ abre el teclado y lo que escribes llega al juego (nombres
  de criaturas, planetas, la consola de trucos). Funcionan tildes, ñ, dictado por voz y la tecla de borrar. Sin
  autocorrector, para que no cambie los nombres inventados. Un teclado físico también sigue funcionando.
- **Tamaño de pantalla** (Ajustes ⚙ → Gráficos → *Tamaño de pantalla*): achica un poco la imagen del juego, centrada y sin
  deformarla, para que nada quede debajo de las esquinas redondeadas o del agujero de la cámara. Se aplica al
  reiniciar el juego.
- **Ajustes y controles en español** cuando el teléfono está en español.
- **Nombre propio**: se instala como **Spore Collection GOG**, al lado de la app oficial, sin reemplazarla.

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

Las partidas guardadas y los ajustes de esta app van en `Android/data/com.sporere.game.freed/files`, aparte de los
de la app oficial.

## Versión del juego

GOG Spore Collection 3.1.0.22, `SporebinEP1/SporeApp.exe` (sha256 en `re/target.sha256`). Otras versiones no
funcionan.

## Estado

- Funcionan todas las etapas (célula, criatura, tribu, civilización, espacio), los editores, las partidas guardadas
  y el sonido.
- Los controles táctiles cambian según la etapa (con una pista corta): tocar = clic, mantener = clic derecho, un dedo
  gira la cámara (criatura, espacio) o mueve el mapa (tribu, civilización), dos dedos giran, pellizcar hace zoom, y en
  la etapa de criatura hay un joystick y botones de habilidades. Todos los botones se pueden mover
  (Ajustes → Mover botones).
- Ajustes: resolución, nitidez, límite de FPS 30-120, calidad, tamaño de pantalla, tamaño y opacidad de los botones.
- Problemas conocidos: todavía no aparece Aventuras Espaciales en el menú principal, las excepciones de C++ del juego
  no se manejan (algunos perfiles con mods se cierran), y "Salir" en el menú de pausa cierra la app (es la salida
  normal del juego).

## Cómo se compila el APK de este fork

Cada cambio que se sube a `android/` hace que GitHub Actions compile los APK solo (`.github/workflows/fork-apk.yml`)
y los publique en Releases. No necesita los archivos del juego: toma el código Java de este repositorio y las
librerías nativas del último release oficial de VinTeRuS (`android/build-fork-apk.sh`). Por eso, por ahora, las
mejoras de este fork son solo en la parte Java (controles, teclado, ajustes, pantalla).

Para cambiar la parte nativa (el motor) hace falta la compilación completa del port original, con el juego
instalado y el Android NDK: ver [README en inglés](README.en.md#building-rough).

## Qué hay en el repositorio

El código del port: el host Win32/D3D9, la app de Android (Java, controles táctiles, ajustes), los scripts de
compilación, el recompilador (`third_party/x86recomp`) y parches para DXVK. No hay archivos del juego ni código del
juego recompilado: ese código se genera al compilar, a partir de tu propia copia del juego.

## Legal

Necesitas tu propia copia comprada de Spore. Spore es una marca registrada de Electronic Arts; este proyecto no está
afiliado a EA ni a Maxis.
