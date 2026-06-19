#!/bin/sh
set -eu

PORT=19090
ROOT="tmp-smoke"

rm -rf "$ROOT" buckets
mkdir -p "$ROOT/local/dir" "$ROOT/out"

printf "hola mundo\n" > "$ROOT/local/a.txt"
printf "contenido anidado\n" > "$ROOT/local/dir/b.txt"

./aws-s3_server "$PORT" > "$ROOT/server.log" 2>&1 &
SERVER_PID=$!
trap 'kill "$SERVER_PID" 2>/dev/null || true; rm -rf "$ROOT"' EXIT
sleep 1

./aws-s3 --port "$PORT" mb s3://demo
./aws-s3 --port "$PORT" cp "$ROOT/local/a.txt" s3://demo/
./aws-s3 --port "$PORT" cp "$ROOT/local" s3://demo/copia/ --recursive
./aws-s3 --port "$PORT" ls s3://demo/
./aws-s3 --port "$PORT" cp s3://demo/a.txt "$ROOT/out/a.txt"
cmp "$ROOT/local/a.txt" "$ROOT/out/a.txt"
./aws-s3 --port "$PORT" cp s3://demo/copia/ "$ROOT/out/copia" --recursive
cmp "$ROOT/local/dir/b.txt" "$ROOT/out/copia/dir/b.txt"
./aws-s3 --port "$PORT" cp s3://demo/copia/ s3://demo/copia2/ --recursive
./aws-s3 --port "$PORT" mv s3://demo/a.txt s3://demo/movido.txt
./aws-s3 --port "$PORT" mv s3://demo/copia2/ s3://demo/copia3/ --recursive
./aws-s3 --port "$PORT" cp s3://demo/copia3/dir/b.txt "$ROOT/out/b2.txt"
cmp "$ROOT/local/dir/b.txt" "$ROOT/out/b2.txt"
./aws-s3 --port "$PORT" rm s3://demo/copia/ --recursive
./aws-s3 --port "$PORT" sync "$ROOT/local" s3://demo/sync/ --delete
./aws-s3 --port "$PORT" sync s3://demo/sync/ "$ROOT/out/sync"
cmp "$ROOT/local/dir/b.txt" "$ROOT/out/sync/dir/b.txt"
./aws-s3 --port "$PORT" rb s3://demo --force

echo "Pruebas smoke completadas"
