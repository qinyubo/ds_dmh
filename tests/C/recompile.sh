#!/bin/bash

cd /cac/u01/yq47/Documents/dataspaces_dmh_caper
make clean
make
cd /cac/u01/yq47/Documents/dataspaces_dmh_caper/tests/C
echo "Recompile done!"
bash /cac/u01/yq47/Documents/dataspaces_dmh_caper/tests/C/cleanall.sh
