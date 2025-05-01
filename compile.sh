
cd src
mkdir -p ../bin

gcc main.c -o ../bin/trayactions \
    `pkg-config --cflags --libs gtk+-3.0 appindicator3-0.1 json-c`


#Run app
if [ $? -ne 0 ]; then
    echo "Compilation failed."
    exit 1
fi
echo "Compilation successful."
cd ..

# Run the application
chmod +x bin/trayactions
./bin/trayactions 