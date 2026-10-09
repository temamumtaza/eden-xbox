# Xbox Series X|S: documentación del fork

Port UWP de Eden con backend Direct3D 12, compartido con las pruebas en PC.
La rama de trabajo es `xbox`; las reglas del proyecto están en [AGENTS.md](../../AGENTS.md).

## Arranque y despliegue

- [Guía rápida para macOS](../../README-macos.md): ejecutar el build en Actions, instalarlo y recopilar logs.
- [Cuaderno del proyecto](xbox_internal.md): estado, decisiones, trampas y resultados de pruebas.
- [Compilación UWP](uwp_build.md): build actual de la app completa y el renderer D3D12.
- [Despliegue en Series](xbox_deploy.md): instalación macOS-to-Xbox y diagnóstico de arranque.
- [Mantenimiento de CI](ci_macos_maintenance.md): signing, versiones, rollback y sincronización upstream.

## Diseño y funciones

- [Direct3D 12, fase 3](xbox_d3d12_phase3.md): scheduler, recursos, cachés y fences.
- [Direct3D 12, fase 4](xbox_d3d12_phase4.md): pipelines y shaders del guest.
- [Audio XAudio2](xbox_audio.md): salida de audio para PC y Series.
- [Biblioteca e interfaz](xbox_frontend.md): carátulas, navegación, mandos y teclado.
- [Carpetas y almacenamiento USB](xbox_rom_storage.md): acceso UWP persistente y apertura de juegos sin copiarlos.

## Rendimiento y evidencia

- [Rendimiento](xbox_performance.md): perfiles, optimizaciones y prioridades pendientes.
- [Memoria de texturas](xbox_texture_memory.md): packing, readbacks, staging y comparación con Vulkan.
- [Revisión de logs del 30 de septiembre de 2026](xbox_log_review_2026-09-30.md).

Los resultados históricos conservan sus límites y pendientes. Un build correcto o
un arranque en PC no certifica gameplay, rendimiento ni funcionamiento en Series.
