# so-proyecto3

Implementacion en C de una simulacion basica de AWS S3 para el tercer
proyecto de Principios de Sistemas Operativos.

## Compilacion

```sh
make
```

Genera dos ejecutables:

- `aws-s3_server`: servidor de buckets.
- `aws-s3`: cliente de linea de comandos.

## Ejecucion

En una terminal:

```sh
./aws-s3_server 9090
```

En otra terminal:

```sh
./aws-s3 mb s3://demo
./aws-s3 cp archivo.txt s3://demo/
./aws-s3 ls s3://demo/
./aws-s3 cp s3://demo/archivo.txt ./
./aws-s3 rm s3://demo/archivo.txt
./aws-s3 rb s3://demo --force
```

Tambien se puede indicar host y puerto:

```sh
./aws-s3 --host 127.0.0.1 --port 9090 ls
```

## Comandos soportados

- `aws-s3 ls [s3://bucket/prefix]`
- `aws-s3 mb s3://bucket`
- `aws-s3 cp origen destino [--recursive]`
- `aws-s3 mv origen destino`
- `aws-s3 rm s3://bucket/key [--recursive]`
- `aws-s3 sync origen destino [--delete]`
- `aws-s3 rb s3://bucket [--force]`

`cp`, `mv` y `sync` soportan rutas locales y rutas `s3://`.

## Almacenamiento

Los buckets se guardan en el directorio `buckets/`. Cada bucket es un unico
archivo con extension `.bucket`. Al inicio del archivo se almacena un bloque
de directorio con:

- Entradas de objetos: clave, offset y tamano.
- Lista de espacios libres generados por borrados o reemplazos.

El contenido binario de cada objeto se guarda secuencialmente despues del
bloque de directorio. Cuando un objeto se reemplaza con un tamano diferente,
el espacio anterior queda disponible para reutilizarse con primer ajuste.

## Pruebas

```sh
make test
```

La prueba crea un bucket, copia archivos, descarga contenido, mueve objetos,
borra prefijos recursivamente, sincroniza directorios y elimina el bucket.
