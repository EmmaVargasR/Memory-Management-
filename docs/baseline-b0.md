# Baseline B0: conjunto completo residente antes de medir

## Contrato

--baseline b0 exige cache-size == working-set: el payload de la cache contiene
el 100% del conjunto. Se rechazan capacidades menores y mayores para fijar una
configuracion inequívoca. --baseline none es el valor predeterminado y conserva
la cache inicialmente vacia y las pruebas anteriores.

B0 significa caliente en la cache logica de usuario. No garantiza que todos los
bytes esten simultaneamente en las caches L1/L2 del procesador. Es una referencia
empirica de accesos sin fallos, no una cota matematica universal de tiempos.

## Fases

1. Crear respaldos, cache, bitmap opcional y almacenamiento de muestras.
2. Leer 8 bytes del comienzo de cada bloque del conjunto, una sola vez y en
   orden de ID. Cada falta carga el bloque completo mediante el respaldo elegido.
   Asi se materializan datos y paginas del payload; no es solo reservar memoria.
3. Guardar los contadores del calentamiento y comprobar que cada bloque se
   cargo una vez, sin aciertos, expulsiones ni errores. Un fallo aborta sin
   medicion valida ni exportacion de muestras.
4. Reiniciar observaciones de la cache: solicitudes, aciertos, fallos, errores,
   expulsiones y cobertura. Se preservan payload, entradas hash, cantidad de
   residentes y orden LRU. No se borran datos ni se desconecta el respaldo.
5. Guardar los contadores acumulados del respaldo para informar sus diferencias.
6. Empezar el reloj de medicion y ejecutar la carga original con su semilla.
   El calentamiento no consume numeros aleatorios, muestras, checksum ni hash.
7. Validar que requests == hits == accesses, misses == evictions == load_errors
   == 0 y que no crecieron los contadores de solicitudes/bytes del respaldo.
   Si no se cumple, devolver error y no exportar CSV.
8. Exportar y resumir solo las observaciones medidas, fuera del bucle cronometrado.

warmup_elapsed_seconds describe el bucle de carga, sin asignacion inicial ni
reinicio posterior. elapsed_seconds describe solo el bucle de medicion, con el
mismo alcance de instrumentacion documentado en metricas.md.

## Campos

- baseline=b0, cache_start=warm_full y b0_verified=1 identifican B0 validado.
- warmup_requests, warmup_hits, warmup_misses, warmup_evictions,
  warmup_load_errors corresponden exclusivamente al calentamiento.
- requests, hits, misses, evictions, load_errors son de la fase medida.
- warmup_tcp_requests / warmup_tcp_payload_bytes y warmup_sd_requests /
  warmup_sd_read_calls / warmup_sd_read_bytes informan E/S de calentamiento.
- tcp_requests / tcp_payload_bytes y sd_requests / sd_read_calls / sd_read_bytes
  son diferencias de contadores durante la medicion. En B0 deben ser cero.
- tcp_connections conserva el alcance de todo el proceso: puede ser 1 aunque
  las solicitudes medidas sean cero. El handshake y calentamiento usan esa
  conexion. No se abre una conexion nueva al comenzar a medir.
- latency_all y latency_hit tienen accesses muestras y resumenes identicos.
  latency_miss y latency_backend tienen count=0 y tiempos na. Con --metrics off
  se valida igualmente B0, pero no se calculan latencias ni utilizacion.
- Sin B0, warmup_* es cero y b0_verified=na (no aplicable); el comportamiento
  anterior permanece igual. No se cambia la duracion de las trazas frias.

## Utilizacion durante una ventana de medicion

El bitmap de calentamiento se limpia conservando los bloques. Los bytes del
calentamiento no cuentan como demanda medida. usage_resident_blocks incluye
tambien bloques precargados que no fueron consultados durante la medicion.
Por tanto, en B0, resident_blocks no equivale al numero de fallos medidos.

block_utilization_evicted=na es correcto: no hubo expulsiones observadas.
block_utilization_resident y all_loaded describen cobertura de los bloques
residentes al comienzo o cargados durante la ventana. all_loaded conserva su
nombre por compatibilidad; no significa "bloques cargados por fallos medidos"
cuando hay precarga. block_usage_window y block_usage_population lo explicitan.

Ejemplo con B=4096, W=cache=16384 y 3 lecturas secuenciales: se precargan
4 bloques, luego se miden 3 aciertos. La cobertura medida es 24 bytes entre los
16384 residentes, no 24+32 bytes ni una utilizacion del 100% por el calentamiento.

## Ejemplo Raspberry y microSD

```bash
./build/client --baseline b0 --backend sd --file build/data/dataset-16k.bin \
  --pattern sequential --block-size 4096 --cache-size 16384 \
  --working-set 16384 --accesses 4096
```

Esperado: warmup_misses=4, warmup_sd_read_bytes=16384, requests=hits=4096,
misses=0, miss_rate=0, evictions=0, sd_requests=sd_read_calls=sd_read_bytes=0.
La traza y checksum coinciden con la misma carga sin B0. Los tiempos no tienen
un valor prefijado y no deben compararse por igualdad entre ejecuciones.

## Interpretacion para AMAT

Con m=0 la media individual observada corresponde a t_hit de esta configuracion
instrumentada. No usar el tiempo de calentamiento en esa media, ni sustituirla
por elapsed_seconds/requests, que incluye generador y checksum. No restar
latencias na de grupos vacios. Como el tamano de cache afecta condiciones de
busqueda y localidad, B0 no demuestra que t_hit sea constante para todas las
presiones de cache. Esa aproximacion debe evaluarse en el modelo posterior.

La red sigue siendo esencial para precargar B0 cuando se elige TCP; no participa
en las lecturas medidas. El experimento principal con presion de cache conserva
--baseline none. B0 es una referencia adicional a las 72 configuraciones
principales, no reemplaza las comparaciones red frente a microSD.

## Pruebas

make test-b0: pruebas C del reinicio de observaciones sin perder datos ni LRU y
ocho pruebas Python (patrones/semillas, traza, CSV, cobertura, cuatro tamanos B,
metricas desactivadas, argumentos invalidos, TCP y aborto de calentamiento).
make test-sd-b0: valida los tres patrones sobre O_DIRECT, carga inicial y ausencia
de E/S medida. make test, make test-sd, make ubsan y make test-sd-ubsan incorporan
los nuevos casos y conservan las suites anteriores.
