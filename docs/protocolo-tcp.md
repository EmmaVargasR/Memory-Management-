# Servicio de bloques TCP, version 1

El servidor reserva y llena el conjunto de trabajo en RAM antes de anunciar
READY. Cada byte en la direccion absoluta A vale A % 251, igual que en el
respaldo sintetico. Un GET copia bytes ya existentes; no genera bloques.

El cliente mantiene una conexion por ejecucion, realiza un saludo antes de
medir y solicita un bloque completo por cada fallo de cache. No reconecta ni
cambia de respaldo silenciosamente ante errores. El servidor atiende un
cliente a la vez; al desconectarse acepta el siguiente. Ambos extremos usan
TCP_NODELAY. No se requieren privilegios administrativos para estos puertos.

## Formato

Cada cabecera tiene 32 bytes. Todos los enteros usan orden big endian.
No se transmiten estructuras de C directamente.

| Desplazamiento | Bytes | Campo |
| --- | --- | --- |
| 0 | 4 | Magic: 0x4d4d4733 |
| 4 | 4 | Version: 1 |
| 8 | 4 | Tipo: HELLO=1, GET=2, HELLO_REPLY=3, GET_REPLY=4 |
| 12 | 4 | Estado: OK=0, BAD_CONFIG=1, BAD_REQUEST=2, BAD_BLOCK=3 |
| 16 | 8 | Primer argumento |
| 24 | 8 | Segundo argumento |

HELLO contiene tamano de bloque y bytes del conjunto de trabajo. El servidor
exige que coincidan con su configuracion y los devuelve en HELLO_REPLY.
GET contiene ID del bloque y cero. GET_REPLY contiene ID y longitud del
contenido; despues vienen exactamente B bytes si el estado es OK. Un error
no lleva contenido y cierra la sesion. Un ID valido cumple 0 <= ID < W/B.

Las funciones de E/S repiten send/recv hasta completar los bytes previstos,
gestionan EINTR y rechazan una desconexion prematura. El cliente verifica el
tipo, estado, ID y longitud de las respuestas. MSG_NOSIGNAL evita terminar el
proceso por SIGPIPE. La conexion tiene espera de hasta 5 segundos por poll;
cada operacion bloqueante de socket tiene timeout de 10 segundos. Estos
timeouts no son un limite global para la ejecucion.

## Prueba manual dentro del contenedor

Terminal 1:

```bash
./build/server --bind 127.0.0.1 --port 18083 --block-size 4096 --working-set 16384
```

Terminal 2:

```bash
./build/client --backend tcp --host 127.0.0.1 --port 18083 \
  --block-size 4096 --cache-size 8192 --working-set 16384 --accesses 4096
```

Esperado: 4096 lecturas, 4088 hits, 8 misses, 6 expulsiones, 1 conexion,
8 peticiones TCP y 32768 bytes de contenido TCP. El saludo y las cabeceras
no se cuentan en tcp_payload_bytes. El checksum debe coincidir con el mismo
comando usando --backend synthetic.

Detener el servidor con Ctrl+C despues de completar las lecturas. El sistema
operativo recupera su memoria y sockets. El servidor es un proceso de larga
duracion; las pruebas lo terminan por senal al finalizar cada caso.

## Alcance de la medicion

El reloj del cliente empieza despues de conectar, completar el saludo y
reservar la cache, y termina despues del bucle. Incluye el generador de
direcciones, cache, transferencias y checksum. No es todavia instrumentacion
individual de latencias ni validacion del modelo AMAT. Las pruebas por
127.0.0.1 verifican integracion; no representan la red Raspberry-VM. La cache
empieza vacia y las corridas incluyen fallos de arranque.

## Pruebas

make test conserva las pruebas anteriores y agrega siete pruebas TCP:
equivalencia para cuatro tamanos de bloque, rechazo de configuraciones
incompatibles, cabeceras fragmentadas y bytes exactos, ID fuera de rango,
cabecera truncada, contenido truncado y servidor ausente. Usan puertos
efimeros de localhost. make sanitize repite los casos con ASan/UBSan.
