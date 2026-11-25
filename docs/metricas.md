# Latencia y utilizacion de bloques

## Opciones

Las metricas estan activadas por defecto. --metrics off desactiva los relojes
por acceso, el almacenamiento de muestras y el bitmap de utilizacion. Conserva
los resultados de lectura y la traza. --latency-csv ARCHIVO exporta las muestras
individuales; exige metricas activadas y nunca sobrescribe un archivo existente.
El directorio padre debe existir. El CSV y su resumen de configuracion en stdout
deben guardarse juntos para identificar patron, semilla, B, capacidad y respaldo.

## Que mide cada reloj

Se usa CLOCK_MONOTONIC_RAW y se informa en nanosegundos. El generador de
direcciones, trace_hash, hot20 y checksum quedan fuera del intervalo individual.

- latency_all: duracion de todas las llamadas cache_read_ex satisfactorias.
- latency_hit: mismo intervalo, solo cuando el bloque ya estaba en cache.
- latency_miss: mismo intervalo, solo cuando hubo que cargar el bloque.
- latency_backend: duracion del callback que carga el bloque, solo en fallos.
  En TCP incluye envio, espera y recepcion completa; en SD incluye pread y sus
  validaciones; en sintetico incluye la generacion de bytes. No incluye la copia
  del staging a cache ni la actualizacion LRU. Es latencia de software del
  respaldo, no una medicion aislada del dispositivo fisico o del RTT.

El intervalo de cache incluye busqueda, reemplazo/copia cuando corresponde,
memcpy de los 8 bytes y actualizacion del bitmap. En fallos tambien incluye
la instrumentacion anidada del reloj del respaldo. Los relojes, la cobertura
y las muestras tienen costo. No se resta una constante de overhead ni se
afirma que sea una latencia de hardware pura. Cambios de planificacion del SO
tambien pueden aparecer en las colas. No eliminar outliers sin justificarlo.

elapsed_seconds y accesses_per_second siguen describiendo el bucle completo:
generador, respaldos, checksum, hashes y metricas activadas. Preparacion,
asignacion de muestras, ordenacion, resumen y exportacion CSV quedan fuera.
La media individual no debe confundirse con elapsed_seconds/requests.
--metrics off permite estudiar el costo global de instrumentacion, pero sus
tiempos no deben mezclarse con las latencias individuales de corridas activadas.

## Resumenes exactos y memoria

Para cada grupo se imprimen count, sum_ns, mean_ns, min_ns, p50_ns, p95_ns,
p99_ns y max_ns. Los percentiles usan rango mas cercano: ordenar n observaciones
y elegir la posicion ceil(p*n), contando desde 1. Por ejemplo, para [10,20]
p50=10 y p95=p99=20. No se interpola ni se estima mediante histogramas.
Un grupo vacio tiene count=0 y valores na: no se inventa una latencia cero.

Se reserva una muestra por acceso antes del bucle y se tocan sus paginas.
metrics_sample_bytes informa esa reserva. Durante el resumen se necesita
temporalmente hasta latency_summary_scratch_bytes adicionales (8*requests),
ademas de los recursos internos de la biblioteca. La reserva depende del numero
de accesos; si falla se termina con error, sin omitir muestras. Con la ABI
habitual de 64 bits las muestras ocupan 24 bytes por acceso (24 MB por millon).
Los contadores y las muestras no forman parte de cache_payload_bytes.

Para el modelo posterior, la media de todos los accesos es una observacion
instrumentada de AMAT. Como identidad de control:

    mean_all = (hits*mean_hit + misses*mean_miss) / requests

Si un grupo esta vacio usar su suma cero, no multiplicar un valor na. mean_miss
es la duracion completa de un acceso fallido, no directamente la penalizacion
adicional T_miss de la ecuacion t_hit + m*T_miss. No sumarlas sin ajustar esa
definicion. Los tiempos del callback ayudaran a estudiar el costo del respaldo;
todavia faltan calibracion, baseline caliente y validacion del modelo.

## Bytes distintos por residencia

La cobertura se registra con un bit por byte del bloque (aproximadamente
12.5% adicional del payload; usage_bitmap_bytes da la reserva exacta).
Se inicializa antes de medir. Cada entrada mantiene su conteo de bytes distintos.
Leer repetidamente el mismo byte lo cuenta una sola vez durante esa residencia.
Las lecturas superpuestas cuentan la union de sus intervalos. La cobertura se
reinicia solo al cargar un bloque nuevo, aunque su ID se haya visitado antes.

Al expulsar un bloque se suma su cobertura terminada. Al finalizar se informan
por separado los bloques aun residentes: sus vidas estan incompletas y no deben
tratarse como expulsiones. Un fallo de carga conserva tanto los datos y LRU
como la cobertura; no agrega una expulsion ficticia.

- usage_evicted_blocks / usage_evicted_unique_bytes: residencias terminadas.
- usage_resident_blocks / usage_resident_unique_bytes: estado parcial al final.
- block_utilization_evicted: bytes distintos expulsados / (expulsiones*B).
- block_utilization_resident: bytes distintos residentes / (residentes*B).
- block_utilization_all_loaded: suma de ambos / (cargas satisfactorias*B).

Sin bloques en un grupo la fraccion es na. all_loaded incluye vidas incompletas;
es un dato descriptivo adicional. Para el requisito "antes de expulsion" usar
evicted y registrar cuantas expulsiones lo respaldan. Estas fracciones tienen
rango [0,1]; no son hit rate, ocupacion de cache ni hot20_access_fraction.
En esta version todos los bloques tienen igual B, de modo que la fraccion de
sumas equivale al promedio de utilizacion entre las residencias del grupo.

Ejemplo: B=16, cache=16, working-set=32, accesos secuenciales a 0,8,16.
El primer bloque se expulsa tras usar sus 16 bytes. El segundo sigue residente
con 8 bytes usados: evicted=1.0, resident=0.5, all_loaded=0.75.

## CSV

Columnas: access_index,was_hit,access_ns,backend_ns. Indices desde cero; hit=1,
miss=0. En aciertos backend_ns=0 porque no se invoco el respaldo. Solo se exporta
una ejecucion que completo todas sus lecturas. Un error de escritura devuelve
estado fallido y elimina el archivo parcial creado por esa ejecucion.
Se exporta despues del bucle, evitando E/S por muestra dentro de la medicion.
Conservar el resumen de stdout y las versiones de plataforma junto al archivo.

## Pruebas

make test-metrics ejecuta tests C con percentiles y medias conocidos, pruebas
de cobertura con repeticion/solapamiento, un modelo de LRU/cobertura independiente
de 3000 accesos y siete pruebas Python. Estas recalculan los resumenes desde el
CSV, comprueban que apagar metricas no cambie la carga y validan TCP local.
make test-sd-metrics verifica los tres patrones sobre microSD con O_DIRECT.
make test y make test-sd incorporan estos casos conservando las pruebas previas.
make ubsan y make test-sd-ubsan los repiten con UndefinedBehaviorSanitizer.
No se exige que una lectura tarde menos de un umbral ni que un medio sea mas
rapido que otro; esas comparaciones pertenecen al experimento posterior.
