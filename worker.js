// Pocket Windows - Bochs WASM Worker
// Runs Bochs x86-64 emulator in a Web Worker (single-threaded, no SharedArrayBuffer)

let bochsModule = null;

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
        console.log('[Worker] Received init message');

        const isoBytes = msg.isoBytes;
        const hddBytes = msg.hddBytes;
        const biosBytes = msg.biosBytes;
        const vgabiosBytes = msg.vgabiosBytes;
        
        if (!isoBytes || !hddBytes || !biosBytes || !vgabiosBytes) {
            console.error('[Worker] Missing binary payload files for initialization!');
            return;
        }

        startBochs(isoBytes, hddBytes, biosBytes, vgabiosBytes);
    } else if (msg.type === 'keydown') {
        if (bochsModule && bochsModule._bx_wasm_key_event) {
            bochsModule._bx_wasm_key_event(msg.scancode, true);
        }
    } else if (msg.type === 'keyup') {
        if (bochsModule && bochsModule._bx_wasm_key_event) {
            bochsModule._bx_wasm_key_event(msg.scancode, false);
        }
    } else if (msg.type === 'mousemove') {
        if (bochsModule && bochsModule._bx_wasm_mouse_event) {
            bochsModule._bx_wasm_mouse_event(msg.deltaX || 0, msg.deltaY || 0, msg.buttons || 0);
        }
    } else if (msg.type === 'mic_data') {
        // Microphone PCM audio chunks received from main thread getUserMedia() stream
        if (bochsModule && bochsModule._bx_wasm_mic_input && msg.pcmData) {
            const ptr = bochsModule._malloc(msg.pcmData.byteLength);
            bochsModule.HEAPU8.set(new Uint8Array(msg.pcmData), ptr);
            bochsModule._bx_wasm_mic_input(ptr, msg.pcmData.byteLength);
            bochsModule._free(ptr);
        }
    }
};

async function startBochs(isoBytes, hddBytes, biosBytes, vgabiosBytes) {
    console.log('[Worker] Starting Bochs with Audio & Microphone bridges...');
    
    try {
        importScripts('./bochs.js');
        
        bochsModule = await createBochsModule({
            onFrame: handleFrame,
            onDimensionChange: handleDimensionChange,
            onAudioOutput: handleAudioOutput,
            onRequestMic: handleRequestMic,
            print: handlePrint,
            printErr: handlePrintErr
        });
        
        console.log('[Worker] Bochs module created');
        
        const FS = bochsModule.FS;
        FS.mkdir('/pack');

        FS.writeFile('/pack/BIOS-bochs-latest', new Uint8Array(biosBytes));
        FS.writeFile('/pack/VGABIOS-lgpl-latest', new Uint8Array(vgabiosBytes));
        
        const hddArray = new Uint8Array(hddBytes);
        FS.writeFile('/pack/hdd.img', hddArray);
        
        const isoArray = new Uint8Array(isoBytes);
        FS.writeFile('/pack/boot.iso', isoArray);

        const bochsrc = `
# Bochs WASM Configuration - Optimized Execution Engine with SB16 Audio
cpu: count=1, ips=15000000, quantum=16, reset_on_triple_fault=1, ignore_bad_msrs=1
clock: sync=none, time0=local
megs: 512

romimage: file=/pack/BIOS-bochs-latest, options=fastboot
vgaromimage: file=/pack/VGABIOS-lgpl-latest

pci: enabled=1, chipset=i440fx
vga: extension=vbe, update_freq=60

sb16: enabled=1, wavemode=1, dmatimer=200000, log=none

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

        bochsModule.callMain(['-f', '/pack/bochsrc.txt', '-q']);
        
    } catch (err) {
        console.error('[Worker] Error starting Bochs:', err);
        self.postMessage({ type: 'error', message: err.message });
    }
}

function handleFrame(data, width, height) {
    if (data instanceof ImageData) {
        self.postMessage({
            type: 'frame',
            imageData: data,
            width: data.width,
            height: data.height
        });
    } else if (data instanceof Uint8Array || data instanceof Uint8ClampedArray) {
        const copy = new Uint8ClampedArray(data);
        const imgData = new ImageData(copy, width, height);
        self.postMessage({
            type: 'frame',
            imageData: imgData,
            width: width,
            height: height
        });
    }
}

function handleAudioOutput(pcmBytes) {
    if (!pcmBytes) return;
    const copy = new Uint8Array(pcmBytes);
    self.postMessage({
        type: 'audio',
        samples: copy.buffer
    }, [copy.buffer]);
}

function handleRequestMic(enable) {
    console.log(`[Worker] Guest requested microphone: ${enable}`);
    self.postMessage({
        type: 'request_mic',
        enable: enable
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
