// Pocket Windows - Bochs WASM Worker
// Runs Bochs x86-64 emulator in a Web Worker (single-threaded, using WORKERFS zero-copy file streaming)

let bochsModule = null;

// Handle messages from main thread
self.onmessage = function(e) {
    const msg = e.data;
    
    if (msg.type === 'init') {
        console.log('[Worker] Received init message with payload files');

        const isoFile = msg.isoFile || msg.isoBytes;
        const hddBytes = msg.hddBytes;
        const biosBytes = msg.biosBytes;
        const vgabiosBytes = msg.vgabiosBytes;
        const ramMegs = msg.ramMegs || 512;
        const bootOrder = msg.bootOrder || 'disk, cdrom';
        
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
    console.log('[Worker] Starting Bochs with mounted files...');
    
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
        
        if (isoFile instanceof Blob || (typeof File !== 'undefined' && isoFile instanceof File)) {
            FS.mkdir('/cdrom_mount');
            FS.mount(FS.filesystems.WORKERFS, {
                blobs: [{ name: 'boot.iso', data: isoFile }]
            }, '/cdrom_mount');
            console.log('[Worker] WORKERFS mounted CD-ROM');
        } else if (isoFile instanceof ArrayBuffer || ArrayBuffer.isView(isoFile)) {
            FS.writeFile('/pack/boot.iso', new Uint8Array(isoFile));
            console.log('[Worker] Wrote CD-ROM buffer to /pack/boot.iso');
        }

        const heads = 16;
        const spt = 63;
        const bytesPerSector = 512;
        const cylinders = Math.max(1, Math.floor(hddArray.byteLength / (heads * spt * bytesPerSector)));

        const cdromPath = (isoFile instanceof Blob || (typeof File !== 'undefined' && isoFile instanceof File)) ? '/cdrom_mount/boot.iso' : '/pack/boot.iso';

        const bochsrc = `
# Bochs WASM Configuration
cpu: count=1, ips=15000000, reset_on_triple_fault=1, ignore_bad_msrs=1
clock: sync=none, time0=local
megs: ${ramMegs}

romimage: file=/pack/BIOS-bochs-latest, options=fastboot
vgaromimage: file=/pack/VGABIOS-lgpl-latest

pci: enabled=1, chipset=i440fx
vga: extension=vbe, update_freq=60

display_library: wasmcanvas

keyboard: type=mf, serial_delay=200
mouse: enabled=1, type=ps2

ata0: enabled=1, ioaddr1=0x1f0, ioaddr2=0x3f0, irq=14
ata0-master: type=disk, mode=flat, path=/pack/hdd.img, cylinders=${cylinders}, heads=${heads}, spt=${spt}
ata0-slave: type=cdrom, path=${cdromPath}, status=inserted

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

function handleFrame(data, width, height, dx, dy, dw, dh) {
    if (data instanceof ImageData) {
        self.postMessage({
            type: 'frame',
            imageData: data,
            width: data.width,
            height: data.height,
            dirtyX: dx !== undefined ? dx : 0,
            dirtyY: dy !== undefined ? dy : 0,
            dirtyW: dw !== undefined ? dw : data.width,
            dirtyH: dh !== undefined ? dh : data.height
        }, [data.data.buffer]);
    } else if (data instanceof Uint8Array || data instanceof Uint8ClampedArray) {
        const copy = new Uint8ClampedArray(data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength));
        const imgData = new ImageData(copy, width, height);
        self.postMessage({
            type: 'frame',
            imageData: imgData,
            width: width,
            height: height,
            dirtyX: dx !== undefined ? dx : 0,
            dirtyY: dy !== undefined ? dy : 0,
            dirtyW: dw !== undefined ? dw : width,
            dirtyH: dh !== undefined ? dh : height
        }, [copy.buffer]);
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
    if (text && (text.includes('i[') || text.includes('d[') || text.includes('[BIOS]') || text.includes('[VBIOS]') || text.includes('[BXVGA]'))) {
        console.log('[Bochs]', text);
        self.postMessage({ type: 'log', text: text, level: 'info' });
    } else {
        console.error('[Bochs]', text);
        self.postMessage({ type: 'log', text: text, level: 'error' });
    }
}
