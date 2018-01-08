#!/bin/sh
#SBATCH -J DS_test
#SBATCH -o DS_test.%J.stdout
#SBATCH -e DS_test.%J.stderr
##SBATCH -p normal
##SBATCH -p Flat-Quadrant
##SBATCH -p Flat-SNC-4
#SBATCH -p development
#SBATCH -A TG-CCR150034
#SBATCH -N 1
#SBATCH -n 4
#SBATCH -t 00:02:00
#SBATCH --mail-type=ALL
#SBATCH --mail-user=qybo123@gmail.com

DIR=.
CONF_DIMS=8192

rm -f conf cred dataspaces.conf srv.lck

echo "## Config file for DataSpaces
ndim = 2
dims = $CONF_DIMS, $CONF_DIMS

max_versions = 1
lock_type = 2
" > dataspaces.conf
#./dataspaces_server -s 4 -c 72 & sleep 2

#./test_writer DATASPACES 64 3 4 4 4 256 256 256 2 1 > test_writer.out 2>&1 & 
#./test_reader DATASPACES 8 3 4 2 1 256 512 1024 2 2 > test_reader.out 2>&1 &

mpirun -n 1 $DIR/dataspaces_server -s 1 -c 2 >&$DIR/server_$CONF_DIMS.log & sleep 2

mpirun -n 1 $DIR/test_writer DATASPACES 1 2 1 1 $CONF_DIMS $CONF_DIMS 2 1 > $DIR/writer_$CONF_DIMS.log 2>&1 &
mpirun -n 1 $DIR/test_reader DATASPACES 1 2 1 1 $CONF_DIMS $CONF_DIMS 2 2 > $DIR/reader_$CONF_DIMS.log 2>&1 &

#mpirun -n 1 $DIR/test_writer DATASPACES 1 2 1 1 32768 32768 2 1 -d 1 > $DIR/writer_$CONF_DIMS.log 2>&1 &
#mpirun -n 1 $DIR/test_reader DATASPACES 1 2 1 1 32768 32768 2 2 -d 1 > $DIR/reader_$CONF_DIMS.log 2>&1 &

#mpirun -n 1 $DIR/test_writer DATASPACES 1 2 1 1 16384 16384 2 1 -d 1 > $DIR/writer_$CONF_DIMS.log 2>&1 &
#mpirun -n 1 $DIR/test_reader DATASPACES 1 2 1 1 16384 16384 2 2 -d 1 > $DIR/reader_$CONF_DIMS.log 2>&1 &

#mpirun -n 1 $DIR/test_writer DATASPACES 1 2 1 1 256 256 2 1 -d 1 > $DIR/writer_$CONF_DIMS.log 2>&1 & 
#mpirun -n 1 $DIR/test_reader DATASPACES 1 2 1 1 256 256 2 2 -d 1 > $DIR/reader_$CONF_DIMS.log 2>&1 &
#mpirun -n 4 $DIR/test_writer DATASPACES 4 2 4 1 1024 4096 5 1 > $DIR/writer_$CONF_DIMS.log 2>&1 &
#mpirun -n 1 $DIR/test_reader DATASPACES 1 2 1 1 4096 4096 5 2 > $DIR/reader_$CONF_DIMS.log 2>&1 &
#mpirun -n 4 $DIR/test_writer DATASPACES 4 2 4 1 512 2048 5 1 > $DIR/writer_$CONF_DIMS.log 2>&1 &
#mpirun -n 1 $DIR/test_reader DATASPACES 1 2 1 1 2048 2048 5 2 > $DIR/reader_$CONF_DIMS.log 2>&1 &
#mpirun -n 4 $DIR/test_writer DATASPACES 4 2 4 1 256 1024 5 1 > $DIR/writer_$CONF_DIMS.log 2>&1 &
#mpirun -n 1 $DIR/test_reader DATASPACES 1 2 1 1 1024 1024 5 2 > $DIR/reader_$CONF_DIMS.log 2>&1 &
wait
