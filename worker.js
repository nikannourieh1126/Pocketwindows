// Pocket Windows - Bochs WASM Worker
// Runs Bochs x86-64 emulator in a Web Worker (single-threaded, no SharedArrayBuffer)

let bochsModule = null;

self.onmessage = function(e) {
    const msg = e.data;
    if (!msg || !msg.type) return;
    
    if (msg.type === 'init') {
        console.log('[Worker] Received init message');
        const isoBytes = msg.isoBytes;
        const hddBytes = msg.hddBytes;
        const biosBytes = msg.biosBytes;
        const vgabiosBytes = msg.vgabiosBytes;
        
        if (!isoBytes || !hddBytes || !biosBytes || !vgabiosBytes) {
            console.error('[Worker] Missing required binary buffers for init');
            self.postMessage({ type: 'error', message: 'Missing required binary buffers for init' });
            return;
        }

        startBochs(isoBytes, hddBytes, biosBytes, vgabiosBytes);
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

async function startBochs(isoBytes, hddBytes, biosBytes, vgabiosBytes) {
    console.log('[Worker] Starting Bochs initialization...');
    self.postMessage({ type: 'log', text: '[Worker] Starting Bochs initialization...', level: 'info' });
    
    try {
        importScripts('./bochs.js');
        
        bochsModule = await createBochsModule({
            noInitialRun: true,
            onFrame: handleFrame,
            onDimensionChange: handleDimensionChange,
            print: handlePrint,
            printErr: handlePrintErr
        });
        
        console.log('[Worker] Bochs WebAssembly module created successfully');
        self.postMessage({ type: 'log', text: '[Worker] Bochs WebAssembly module created successfully', level: 'info' });
        
        const FS = bochsModule.FS;
        
        try {
            FS.mkdir('/pack');
        } catch (e) {
        }

        FS.writeFile('/pack/BIOS-bochs-latest', new Uint8Array(biosBytes));
        FS.writeFile('/pack/VGABIOS-lgpl-latest', new Uint8Array(vgabiosBytes));
        
        const hddArray = new Uint8Array(hddBytes);
        FS.writeFile('/pack/hdd.img', hddArray);
        
        const totalSectors = Math.floor(hddArray.length / 512) || 1;
        const heads = 16;
        const spt = 63;
        const cylinders = Math.max(1, Math.floor(totalSectors / (heads * spt)));

        const isoArray = new Uint8Array(isoBytes);
        FS.writeFile('/pack/boot.iso', isoArray);

        const bochsrc = `
# Bochs WASM Configuration
cpu: count=1, reset_on_triple_fault=1, ignore_bad_msrs=1
megs: 512

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
