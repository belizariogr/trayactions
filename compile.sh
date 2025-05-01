cd src
mkdir -p ../bin

gcc main.c config.c menu.c utils.c \
    -o ../bin/trayactions \
    `pkg-config --cflags --libs gtk+-3.0 appindicator3-0.1 json-c`

if [ $? -ne 0 ]; then
    echo "Compilation failed."
    exit 1
fi

echo "Compilation successful."
cd ..

chmod +x bin/trayactions
./bin/trayactions