./compile.sh

sudo cp -f bin/trayactions /usr/local/bin/

nohup trayactions > /dev/null 2>&1 &