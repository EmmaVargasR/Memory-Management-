# Respaldo local microSD con O_DIRECT

## Alcance

El respaldo sd lee un archivo normal en el sistema de archivos. En la Raspberry
del proyecto, findmnt identifico /dev/mmcblk0p2, ext4, montado en /, y el proyecto
esta bajo /home/rasp/proyectos/Memory-Management-. El lector no escribe ni abre
el dispositivo de bloques para modificarlo. El nombre sd no detecta el tipo de
hardware: al cambiar de carpeta o equipo se debe volver a registrar el montaje.

## Preparacion y lectura

```bash
python3 scripts/prepare_dataset.py --path build/data/dataset-16k.bin --size 16384
./build/client --backend sd --file build/data/dataset-16k.bin \
  --block-size 4096 --cache-size 8192 --working-set 16384 --accesses 4096
```

El generador crea un archivo nuevo de manera exclusiva, rechaza sobrescribir
uno existente, escribe byte(A)=A%251 y completa flush/fsync antes de terminar.
El archivo coincide con los datos del servidor TCP y del respaldo sintetico.
El SHA256 del archivo se informa para registrar la version de los datos.
Generar el archivo una vez por conjunto de trabajo, fuera del tiempo medido.

El lector abre con O_RDONLY y O_DIRECT, verifica mediante fstat que sea un
archivo regular y que su longitud coincida exactamente con --working-set.
Consulta statx con STATX_DIOALIGN sobre el mismo descriptor. Exige que el kernel
informe requisitos no nulos de alineacion de memoria y de desplazamiento.
Si no se dispone de esta informacion o de lectura directa, termina con error.
No utiliza un respaldo buffered alternativo ni informa una prueba omitida como
aprobada. Los bloques SD deben ser multiplos de 4096 bytes.

La cache ya reserva su bloque temporal con posix_memalign y alineacion 4096.
Antes de pread, el backend comprueba que la direccion del buffer, el
desplazamiento ID*B y la longitud B satisfagan los requisitos del archivo.
Si el archivo requiere una alineacion incompatible con ese buffer, se rechaza
la corrida. Se lee directamente en el bloque temporal existente, sin otra copia
de contenido especifica del respaldo. El reemplazo de la cache ocurre despues
de completar la carga.

Se reintenta pread ante EINTR sin bytes recibidos. Una transferencia corta o
EOF inesperado invalida la carga; no se reintenta el resto con punteros que
puedan quedar desalineados. Los datos validos de la cache se conservan.

## Contadores y resultado esperado

Para el comando de ejemplo: requests=4096, hits=4088, misses=8, evictions=6,
load_errors=0, checksum=4083442, tcp_connections=0, sd_open_direct=1,
sd_alignment_source=statx, sd_requests=8 y sd_read_bytes=32768.
sd_read_calls cuenta intentos de pread y puede aumentar con EINTR. Las
alineaciones en bytes dependen del archivo y deben ser positivas.

sd_open_direct indica que el archivo se abrio con O_DIRECT y se verificaron las
restricciones reportadas. O_DIRECT evita el page cache para la E/S directa
soportada; no elimina las caches internas de la tarjeta o el controlador.
No demuestra por si solo latencia exclusiva de la memoria flash. La metrica
actual incluye el bucle, cache, carga y checksum; excluye abrir y configurar el
respaldo. No es todavia instrumentacion de latencias individuales.

## Pruebas y arquitectura

```bash
make test
make test-sd
make ubsan
make test-sd-ubsan
```

make test conserva las pruebas de cache, cliente y TCP. make test-sd ejecuta
siete pruebas Python y una prueba C con archivo temporal. Las pruebas SD viven
en build/, sobre el mismo sistema de archivos que el proyecto. Necesitan que
ese sistema reporte STATX_DIOALIGN y soporte O_DIRECT. Son una comprobacion
explicita aparte, porque el sistema de archivos de otro host puede no soportarlo.

Las pruebas comparan los cuatro tamanos de bloque con el respaldo sintetico,
verifican datos, conteos, parametros invalidos, archivo ausente o de longitud
incorrecta y que el generador no sobrescriba datos. La prueba C inyecta EOF
truncando su propio archivo temporal, muestra un diagnostico esperado y
comprueba que un miss fallido no expulse un bloque valido.

make ubsan ejecuta las pruebas base con -fsanitize=undefined y
-fno-sanitize-recover=all. make test-sd-ubsan repite la comprobacion SD con esos
binarios. make sanitize conserva ASan/UBSan para los entornos donde funcionan;
no se ha eliminado ASan del proyecto.

En la Raspberry se reprodujo un fallo de inicializacion de ASan incluso con un
programa minimo. Las pruebas base normales y UBSan pasaron en ARM64. Las
pruebas base ASan/UBSan pasaron en el contenedor x86_64. Esto no equivale a
haber aprobado ASan en la Raspberry. Las nuevas pruebas SD deben ejecutarse
en el equipo de destino antes de declarar su resultado.

## Estado de la comunicacion remota

La ejecucion real Raspberry a so-g03 ya obtuvo los conteos esperados mediante
un tunel SSH: listener 127.0.0.1:18084 en la Raspberry, SSH a
172.20.9.55:2222 y destino observado 172.17.0.4:18083. Estas direcciones deben
verificarse al repetir la prueba. No se ha validado acceso TCP directo entre
ambos equipos. El cifrado y procesamiento SSH forman parte del camino remoto
y se deben incluir o controlar al calibrar el modelo.

## Referencias

- https://man7.org/linux/man-pages/man2/open.2.html
- https://man7.org/linux/man-pages/man2/statx.2.html
- https://man7.org/linux/man-pages/man2/pread.2.html
- https://github.com/google/sanitizers/issues/1674
