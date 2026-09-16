// Pocket Windows - Bochs WASM Worker
// Runs Bochs x86-64 emulator in a Web Worker (single-threaded, no SharedArrayBuffer)

let bochsModule = null;
let canvas = null;
let ctx = null;
let animationId = null;

// Hard disk configuration
const HDD_SIZE_MB = 2048; // Default to 2GB to avoid browser memory issues
const HDD_SIZE_BYTES = HDD_SIZE_MB * 1024 * 1024;
const HDD_CYLINDERS = Math.floor(HDD_SIZE_BYTES / (16 * 63 * 512));
const HDD_HEADS = 16;
const HDD_SPT = 63;

console.log(`[Worker] HDD Configuration: ${HDD_SIZE_MB}MB, cylinders=${HDD_CYLINDERS}, heads=${HDD_HEADS}, spt=${HDD_SPT}`);

// Handle messages from main thread
self.onmessage = function(e) {
    const msg = e.data;
    
    if (msg.type === 'init') {
        // Initialize with transferred ISO, HDD, BIOS and VGABIOS data
        console.log('[Worker] Received init message');

        const isoBytes = msg.isoBytes;
        const hddBytes = msg.hddBytes;
        const biosBytes = msg.biosBytes;
        const vgabiosBytes = msg.vgabiosBytes;
        
        if (!isoBytes) {
            console.error('[Worker] No ISO bytes received!');
            return;
        }

        if (!hddBytes) {
            console.error('[Worker] No HDD bytes received!');
            return;
        }

        if (!biosBytes || !vgabiosBytes) {
            console.error('[Worker] Missing biosBytes/vgabiosBytes! Bochs cannot start without them.');
            return;
        }

        console.log(`[Worker] ISO size: ${isoBytes.byteLength} bytes`);
        console.log(`[Worker] HDD size: ${hddBytes.byteLength} bytes`);
        console.log(`[Worker] BIOS size: ${biosBytes.byteLength} bytes`);
        console.log(`[Worker] VGABIOS size: ${vgabiosBytes.byteLength} bytes`);

        // Start Bochs with the images
        startBochs(isoBytes, hddBytes, biosBytes, vgabiosBytes);
    } else if (msg.type === 'keydown') {
        if (bochsModule && bochsModule.onKeyDown) {
            bochsModule.onKeyDown(msg.keyCode, msg.scancode);
        }
    } else if (msg.type === 'keyup') {
        if (bochsModule && bochsModule.onKeyUp) {
            bochsModule.onKeyUp(msg.keyCode, msg.scancode);
        }
    } else if (msg.type === 'mousemove') {
        if (bochsModule && bochsModule.onMouseMove) {
            bochsModule.onMouseMove(msg.x, msg.y, msg.buttons);
        }
    } else if (msg.type === 'mousedown') {
        if (bochsModule && bochsModule.onMouseDown) {
            bochsModule.onMouseDown(msg.button);
        }
    } else if (msg.type === 'mouseup') {
        if (bochsModule && bochsModule.onMouseUp) {
            bochsModule.onMouseUp(msg.button);
        }
    }
};

async function startBochs(isoBytes, hddBytes, biosBytes, vgabiosBytes) {
    console.log('[Worker] Starting Bochs...');
    
    try {
        // Import the Bochs module
        importScripts('./bochs.js');
        
        // Create Bochs module instance
        bochsModule = await createBochsModule({
            onFrame: handleFrame,
            onDimensionChange: handleDimensionChange,
            print: handlePrint,
            printErr: handlePrintErr
        });
        
        console.log('[Worker] Bochs module created');
        
        // Set up virtual filesystem
        const FS = bochsModule.FS;

        // Create /pack directory
        FS.mkdir('/pack');

        // Write BIOS and VGABIOS files (required — bochsrc references them by these exact names)
        console.log('[Worker] Writing BIOS-bochs-latest...');
        FS.writeFile('/pack/BIOS-bochs-latest', new Uint8Array(biosBytes));
        console.log('[Worker] Writing VGABIOS-lgpl-latest...');
        FS.writeFile('/pack/VGABIOS-lgpl-latest', new Uint8Array(vgabiosBytes));
        
        // Write the hard disk image
        console.log('[Worker] Writing HDD image to /pack/hdd.img...');
        const hddArray = new Uint8Array(hddBytes);
        FS.writeFile('/pack/hdd.img', hddArray);
        console.log(`[Worker] HDD image written: ${hddArray.length} bytes`);
        
        // Write the ISO image
        console.log('[Worker] Writing ISO image to /pack/boot.iso...');
        const isoArray = new Uint8Array(isoBytes);
        FS.writeFile('/pack/boot.iso', isoArray);
        console.log(`[Worker] ISO image written: ${isoArray.length} bytes`);

        // Create bochsrc configuration
        const bochsrc = `
# Bochs WASM Configuration
cpu: count=1, cores=1, threads=1, reset_on_triple_fault=1, ignore_bad_msrs=1
megs: 512

romimage: file=/pack/BIOS-bochs-latest, options=fastboot
vgaromimage: file=/pack/VGABIOS-lgpl-latest

display_library: wasmcanvas

keyboard_type: mf, serial_delay=200
mouse: enabled=0

ata0: enabled=1, ioaddr1=0x1f0, ioaddr2=0x3f0, irq=14
ata0-master: type=disk, mode=flat, path=/pack/hdd.img, cylinders=${HDD_CYLINDERS}, heads=${HDD_HEADS}, spt=${HDD_SPT}
ata0-slave: type=cdrom, path=/pack/boot.iso, status=inserted

boot: cdrom
`;
        
        FS.writeFile('/pack/bochsrc.txt', new TextEncoder().encode(bochsrc));
        console.log('[Worker] Bochsrc written');

        // Start Bochs with our config
        console.log('[Worker] Calling callMain with bochsrc...');
        bochsModule.callMain(['-f', '/pack/bochsrc.txt', '-q']);
        console.log('[Worker] callMain returned');
        
    } catch (err) {
        console.error('[Worker] Error starting Bochs:', err);
        self.postMessage({ type: 'error', message: err.message });
    }
}

function handleFrame(imageData) {
    // Send frame data to main thread
    self.postMessage({
        type: 'frame',
        imageData: imageData,
        width: imageData.width,
        height: imageData.height
    }, [imageData.data.buffer]);
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
