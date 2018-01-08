#!/bin/bash
DIR=.
CONF_DIMS=8192

bash $DIR/cleanall.sh

rm -f conf cred dataspaces.conf srv.lck

echo "## Config file for DataSpaces
ndim = 2
dims = $CONF_DIMS, $CONF_DIMS

max_versions = 1
lock_type = 2
" > dataspaces.conf

mpirun -n 1 $DIR/dataspaces_server -s 1 -c 2 >&$DIR/server_$CONF_DIMS.log & sleep 2

mpirun -n 1 $DIR/test_writer DATASPACES 1 2 1 1 $CONF_DIMS $CONF_DIMS 4 1 > $DIR/writer_$CONF_DIMS.log 2>&1 &
mpirun -n 1 $DIR/test_reader DATASPACES 1 2 1 1 $CONF_DIMS $CONF_DIMS 4 2 > $DIR/reader_$CONF_DIMS.log 2>&1 &
