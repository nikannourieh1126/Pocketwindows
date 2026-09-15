// Pocket Windows - Bochs WASM Worker
// Runs Bochs x86-64 emulator in a Web Worker (single-threaded, using WORKERFS zero-copy file streaming)

let bochsModule = null;

// Handle messages from main thread
self.onmessage = function(e) {
    const msg = e.data;
    
    if (msg.type === 'init') {
        console.log('[Worker] Received init message with WORKERFS file object');

        const isoFile = msg.isoFile;
        const hddBytes = msg.hddBytes;
        const biosBytes = msg.biosBytes;
        const vgabiosBytes = msg.vgabiosBytes;
        const ramMegs = msg.ramMegs || 512;
        const bootOrder = msg.bootOrder || 'cdrom';
        
        if (!isoFile || !hddBytes || !biosBytes || !vgabiosBytes) {
            console.error('[Worker] Missing binary payload files for initialization!');
            return;
        }

        startBochs(isoFile, hddBytes, biosBytes, vgabiosBytes, ramMegs, bootOrder);
    } else if (msg.type === 'keydown') {
        const key = msg.scancode !== undefined ? msg.scancode : msg.key;
        if (bochsModule && bochsModule._bx_wasm_key_event && key !== undefined) {
            bochsModule._bx_wasm_key_event(key, true);
        }
    } else if (msg.type === 'keyup') {
        const key = msg.scancode !== undefined ? msg.scancode : msg.key;
        if (bochsModule && bochsModule._bx_wasm_key_event && key !== undefined) {
            bochsModule._bx_wasm_key_event(key, false);
        }
    } else if (msg.type === 'mouse' || msg.type === 'mousemove') {
        const dx = msg.x !== undefined ? msg.x : (msg.deltaX || 0);
        const dy = msg.y !== undefined ? msg.y : (msg.deltaY || 0);
        const btns = msg.buttonState !== undefined ? msg.buttonState : (msg.buttons || 0);
        if (bochsModule && bochsModule._bx_wasm_mouse_event) {
            bochsModule._bx_wasm_mouse_event(dx, dy, btns);
        }
    } else if (msg.type === 'mic_data') {
        if (bochsModule && bochsModule._bx_wasm_mic_input && msg.pcmData) {
            const ptr = bochsModule._malloc(msg.pcmData.byteLength);
            bochsModule.HEAPU8.set(new Uint8Array(msg.pcmData), ptr);
            bochsModule._bx_wasm_mic_input(ptr, msg.pcmData.byteLength);
            bochsModule._free(ptr);
        }
    }
};

async function startBochs(isoFile, hddBytes, biosBytes, vgabiosBytes, ramMegs, bootOrder) {
    console.log(`[Worker] Starting Bochs using WORKERFS zero-copy streaming (ISO size: ${isoFile.size} bytes)...`);
    
    try {
        importScripts('./bochs.js');
        
        bochsModule = await createBochsModule({
            noInitialRun: true,
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
        
        // Mount ISO directly via WORKERFS - zero-copy, lazy sector reading from File/Blob!
        FS.mkdir('/cdrom_mount');
        FS.mount(FS.filesystems.WORKERFS, {
            blobs: [{ name: 'boot.iso', data: isoFile }]
        }, '/cdrom_mount');

        console.log(`[Worker] WORKERFS successfully mounted CD-ROM at /cdrom_mount/boot.iso (File Size: ${isoFile.size} bytes)`);

        // Dynamically compute cylinders based on hard disk image size
        const heads = 16;
        const spt = 63;
        const bytesPerSector = 512;
        const cylinders = Math.max(1, Math.floor(hddArray.byteLength / (heads * spt * bytesPerSector)));

        console.log(`[Worker] Dynamic HDD CHS Geometry: cylinders=${cylinders}, heads=${heads}, spt=${spt} (size: ${hddArray.byteLength} bytes)`);

        const bochsrc = `
# Bochs WASM Configuration - Optimized Execution Engine with SB16 Audio & WORKERFS
cpu: count=1, ips=15000000, reset_on_triple_fault=1, ignore_bad_msrs=1
clock: sync=none, time0=local
megs: ${ramMegs}

romimage: file=/pack/BIOS-bochs-latest, options=fastboot
vgaromimage: file=/pack/VGABIOS-lgpl-latest

pci: enabled=1, chipset=i440fx
vga: extension=vbe, update_freq=60

sb16: enabled=1, wavemode=1, dmatimer=200000, log=none

display_library: wasmcanvas

keyboard: type=mf, serial_delay=200
mouse: enabled=1, type=ps2

ata0: enabled=1, ioaddr1=0x1f0, ioaddr2=0x3f0, irq=14
ata0-master: type=disk, mode=flat, path=/pack/hdd.img, cylinders=${cylinders}, heads=${heads}, spt=${spt}
ata0-slave: type=cdrom, path=/cdrom_mount/boot.iso, status=inserted

boot: ${bootOrder}
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
