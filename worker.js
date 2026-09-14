// Pocket Windows - Bochs WASM Worker
// Runs Bochs x86-64 emulator in a Web Worker (single-threaded, no SharedArrayBuffer)

let bochsModule = null;

// Handle messages from main thread
self.onmessage = function(e) {
    const msg = e.data;
    if (!msg || !msg.type) return;
    
    if (msg.type === 'init') {
        console.log('[Worker] Received init message');
        const isoFile = msg.isoFile;
        const isoBytes = msg.isoBytes;
        const hddBytes = msg.hddBytes;
        const biosBytes = msg.biosBytes;
        const vgabiosBytes = msg.vgabiosBytes;
        
        if ((!isoFile && !isoBytes) || !hddBytes || !biosBytes || !vgabiosBytes) {
            console.error('[Worker] Missing required binary buffers for init');
            self.postMessage({ type: 'error', message: 'Missing required binary buffers for init' });
            return;
        }

        startBochs(isoFile || isoBytes, hddBytes, biosBytes, vgabiosBytes);
    } else if (msg.type === 'keydown') {
        if (bochsModule && bochsModule._bx_wasm_key_event) {
            bochsModule._bx_wasm_key_event(msg.key, false);
        }
    } else if (msg.type === 'keyup') {
        if (bochsModule && bochsModule._bx_wasm_key_event) {
            bochsModule._bx_wasm_key_event(msg.key, true);
        }
    } else if (msg.type === 'mouse') {
        if (bochsModule && bochsModule._bx_wasm_mouse_event) {
            const x = msg.x || 0;
            const y = msg.y || 0;
            const z = msg.z || 0;
            const buttonState = msg.buttonState || 0;
            const absMode = msg.absMode || false;
            bochsModule._bx_wasm_mouse_event(x, y, z, buttonState, absMode);
        }
    }
};

async function startBochs(isoInput, hddBytes, biosBytes, vgabiosBytes) {
    console.log('[Worker] Starting Bochs initialization...');
    self.postMessage({ type: 'log', text: '[Worker] Starting Bochs initialization...', level: 'info' });
    
    try {
        // Import the compiled Bochs WASM glue script
        importScripts('./bochs.js');
        
        // Create Bochs module instance
        bochsModule = await createBochsModule({
            noInitialRun: true,
            onFrame: handleFrame,
            onDimensionChange: handleDimensionChange,
            print: handlePrint,
            printErr: handlePrintErr
        });
        
        console.log('[Worker] Bochs WebAssembly module created successfully');
        self.postMessage({ type: 'log', text: '[Worker] Bochs WebAssembly module created successfully', level: 'info' });
        
        // Set up virtual filesystem
        const FS = bochsModule.FS;
        
        try {
            FS.mkdir('/pack');
        } catch (e) {
            // Ignored if already exists
        }

        // Write BIOS and VGABIOS files
        FS.writeFile('/pack/BIOS-bochs-latest', new Uint8Array(biosBytes));
        FS.writeFile('/pack/VGABIOS-lgpl-latest', new Uint8Array(vgabiosBytes));
        
        // Write Hard Disk image
        const hddArray = new Uint8Array(hddBytes);
        FS.writeFile('/pack/hdd.img', hddArray);
        
        // Compute geometry dynamically for flat hard disk image
        const totalSectors = Math.floor(hddArray.length / 512) || 1;
        const heads = 16;
        const spt = 63;
        const cylinders = Math.max(1, Math.floor(totalSectors / (heads * spt)));

        // Handle ISO file: if File/Blob, use instant lazy sector stream; otherwise write array
        if (isoInput instanceof File || isoInput instanceof Blob) {
            console.log(`[Worker] Setting up zero-copy ISO lazy sector device for ${isoInput.name} (${isoInput.size} bytes)...`);
            self.postMessage({ type: 'log', text: `[Worker] Zero-copy ISO mounted: ${isoInput.name} (${(isoInput.size / (1024*1024)).toFixed(1)} MB)`, level: 'info' });

            const fileReaderSync = new FileReaderSync();
            const isoBlob = isoInput;
            const isoSize = isoBlob.size;

            // Create custom Emscripten FS device for /pack/boot.iso
            const isoDevice = FS.makedev(64, 0);
            FS.registerDevice(isoDevice, {
                open: function(stream) {
                    stream.seekable = true;
                    stream.position = 0;
                },
                close: function(stream) {
                },
                read: function(stream, buffer, offset, length, position) {
                    const pos = (position !== undefined && position !== null) ? position : stream.position;
                    if (pos >= isoSize) return 0;

                    const readLength = Math.min(length, isoSize - pos);
                    if (readLength <= 0) return 0;

                    const slice = isoBlob.slice(pos, pos + readLength);
                    const chunk = new Uint8Array(fileReaderSync.readAsArrayBuffer(slice));
                    buffer.set(chunk, offset);

                    if (position === undefined || position === null) {
                        stream.position += readLength;
                    }
                    return readLength;
                },
                llseek: function(stream, offset, whence) {
                    let newPos = stream.position;
                    if (whence === 0) { // SEEK_SET
                        newPos = offset;
                    } else if (whence === 1) { // SEEK_CUR
                        newPos += offset;
                    } else if (whence === 2) { // SEEK_END
                        newPos = isoSize + offset;
                    }
                    if (newPos < 0) newPos = 0;
                    if (newPos > isoSize) newPos = isoSize;
                    stream.position = newPos;
                    return newPos;
                }
            });

            // Create device file node at /pack/boot.iso
            try {
                FS.unlink('/pack/boot.iso');
            } catch (e) {}
            FS.mkdev('/pack/boot.iso', 0666, isoDevice);

        } else {
            // Small ArrayBuffer / test disk
            const isoArray = new Uint8Array(isoInput);
            FS.writeFile('/pack/boot.iso', isoArray);
        }

        // Generate high-performance bochsrc.txt
        const bochsrc = `
# Bochs WASM High-Performance Configuration
cpu: count=1, ips=500000000, reset_on_triple_fault=1, ignore_bad_msrs=1
megs: 1024

clock: sync=none, time0=local
pci: enabled=1, chipset=i440fx

romimage: file=/pack/BIOS-bochs-latest, options=fastboot
vgaromimage: file=/pack/VGABIOS-lgpl-latest

display_library: wasmcanvas

keyboard: serial_delay=200
mouse: enabled=1, type=ps2

ata0: enabled=1, ioaddr1=0x1f0, ioaddr2=0x3f0, irq=14
ata0-master: type=disk, mode=flat, path=/pack/hdd.img, cylinders=${cylinders}, heads=${heads}, spt=${spt}
ata0-slave: type=cdrom, path=/pack/boot.iso, status=inserted

boot: disk, cdrom
`;
        
        FS.writeFile('/pack/bochsrc.txt', new TextEncoder().encode(bochsrc));
        console.log('[Worker] bochsrc.txt written, invoking callMain...');
        self.postMessage({ type: 'log', text: '[Worker] Invoking Bochs main loop...', level: 'info' });

        // Start Bochs main loop with arguments
        bochsModule.callMain(['-q', '-f', '/pack/bochsrc.txt']);
        
    } catch (err) {
        console.error('[Worker] Error starting Bochs:', err);
        self.postMessage({ type: 'error', message: err.message || String(err) });
    }
}

function handleFrame(imageData) {
    self.postMessage({
        type: 'frame',
        imageData: imageData
    });
}

function handleDimensionChange(width, height) {
    console.log(`[Worker] Dimension change: ${width}x${height}`);
    self.postMessage({
        type: 'dimension',
        width: width,
        height: height
    });
}

function handlePrint(text) {
    console.log('[Bochs]', text);
    self.postMessage({ type: 'log', text: text, level: 'info' });
}

function handlePrintErr(text) {
    console.error('[Bochs]', text);
    self.postMessage({ type: 'log', text: text, level: 'error' });
}
