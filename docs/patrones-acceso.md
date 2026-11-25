# Patrones de acceso reproducibles

## Unidad de acceso

Cada lectura solicita 8 bytes. El conjunto W contiene N=W/8 palabras logicas.
La direccion de la palabra de indice j es j*8. El generador no recibe B,
capacidad de cache ni respaldo; por eso esos factores no alteran las direcciones.
El cliente transforma cada direccion en block_id=address/B y offset=address%B.

## Patrones y opciones

- --pattern sequential conserva el recorrido 0,8,16,... y vuelve al inicio.
- --pattern uniform selecciona cada palabra con igual probabilidad. No exige
  que una corrida finita visite exactamente el mismo numero de veces cada una.
- --pattern zipf selecciona el rango k entre 1 y N con probabilidad proporcional
  a k^(-theta). El rango k se asigna a la direccion (k-1)*8. Los rangos populares
  quedan contiguos en las direcciones bajas; esta decision incorpora localidad
  espacial y debe mantenerse al comparar tamanos de bloque.
- --seed acepta cualquier uint64 decimal, incluido 0. Valor predeterminado 42.
  Sequential informa la semilla pero no la utiliza.
- --zipf-theta acepta 0 < theta <= 2, solo con Zipf. Valor predeterminado 1.1.
  Zipf admite como maximo 2^48 palabras por precision de coma flotante.

No se afirma que theta=1.1 represente una regla 80/20. La concentracion depende
de theta y N. Antes de la campana, fijar el parametro o calibrarlo si se exige
una proporcion especifica. hot20_access_fraction informa la fraccion observada
de accesos a las primeras ceil(N/5) palabras. No es hit rate ni utilizacion de
bloque, y con N pequeno ese subconjunto puede superar ligeramente el 20%.

## Generador pseudoaleatorio

Uniforme y Zipf usan SplitMix64 con aritmetica uint64 definida; no dependen de
rand() de la biblioteca C. Uniforme descarta el tramo incompleto antes de aplicar
modulo N, evitando el sesgo de modulo. Cada objeto tiene su propio estado.

El muestreador Zipf utiliza rechazo con una envolvente integral de memoria
constante. Reserva masa 1 para k=1 y usa densidad x^(-theta) entre 1 y N. Para
x en (k-1,k], esa densidad es al menos k^(-theta). Se propone k=ceil(x) y se
acepta con probabilidad (x/k)^theta. La masa aceptada de cada intervalo es
k^(-theta), que da la distribucion requerida. La integral desde 1 hasta x es
(x^(1-theta)-1)/(1-theta), con limite log(x) para theta=1. Se usan expm1 y
log1p para reducir cancelacion numerica cerca de theta=1.

No hay una tabla CDF o de rangos proporcional a W, que agregaria una reserva
grande de memoria al estudio de cache. La preparacion de la envolvente queda
fuera del bucle medido. La generacion y las verificaciones del bucle siguen
incluidas en el tiempo actual; todavia no es una medida aislada de latencias.

## Huella de las direcciones

trace_hash empieza en 14695981039346656037 y actualiza por acceso:

    hash = (hash XOR address) * 1099511628211 modulo 2^64

Se imprime como 16 digitos hexadecimales. Es una huella de diagnostico, no
criptografica. checksum sigue siendo la suma de bytes leidos. Mismos W, patron,
semilla, parametro y numero de accesos deben dar misma huella y checksum al
cambiar B o respaldo. Los conteos de misses pueden cambiar al cambiar B.

Los calculos Zipf usan libm y operaciones de coma flotante. No prometemos
identidad bit a bit en todos los compiladores y arquitecturas: registrar
versiones y comparar trace_hash. Si las trazas difieren al cambiar plataforma,
usar una plataforma fija para las comparaciones o implementar reproduccion de
trazas guardadas. No activar -ffast-math durante las comparaciones.

## Pruebas

make test y make ubsan conservan los casos anteriores y agregan pruebas C del
generador y seis pruebas Python. Comprobaran repetibilidad, limites de semilla,
alineacion, limites de direcciones, frecuencias frente a la masa de probabilidad,
independencia de B y equivalencia TCP. Las frecuencias usan semillas fijas y
tolerancias amplias, no son una certificacion estadistica del PRNG.

make test-sd y make test-sd-ubsan agregan dos comprobaciones que comparan los
patrones nuevos contra lectura SD con O_DIRECT. Los tests no usan tiempos como
criterio de aprobacion. make sanitize tambien incorpora los patrones para los
entornos donde ASan logra inicializarse.

Ejemplo uniforme:

```bash
./build/client --backend synthetic --pattern uniform --seed 42 \
  --block-size 4096 --cache-size 8192 --working-set 16384 --accesses 4096
```

Ejemplo Zipf sobre microSD:

```bash
./build/client --backend sd --file build/data/dataset-16k.bin \
  --pattern zipf --seed 42 --zipf-theta 1.1 --block-size 4096 \
  --cache-size 8192 --working-set 16384 --accesses 4096
```

## Referencias

Definicion de la distribucion Zipf:
https://commons.apache.org/proper/commons-statistics/commons-statistics-distribution/javadocs/api-1.0/src-html/org/apache/commons/statistics/distribution/ZipfDistribution.html

SplitMix64 y referencia de su estado y mezclador:
https://prng.di.unimi.it/splitmix64.c
