"use strict";

// The device callback transports PCM only. C++ runs in the producer worker.
class PcmAudioProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.queue = new Array(8);
    this.head = 0;
    this.tail = 0;
    this.count = 0;
    this.epoch = 0;
    this.offset = 0;
    this.started = false;
    this.frames = 0;
    this.underrunFrames = 0;
    this.underruns = 0;
    this.starved = false;
    this.reportAt = sampleRate;
    this.recycle = { type: "recycle", buffer: null };
    this.transfer = [null];
    this.port.onmessage = (event) => {
      if (event.data?.type === "prime") {
        this.started = false;
        this.epoch++;
        this.producer?.postMessage({ type: "prime", epoch: this.epoch });
        while (this.count) {
          const packet = this.queue[this.head];
          this.recycle.buffer = this.transfer[0] = packet.pcm.buffer;
          this.producer.postMessage(this.recycle, this.transfer);
          this.queue[this.head] = null;
          this.head = (this.head + 1) % this.queue.length;
          this.count--;
        }
        this.head = this.tail = this.offset = 0;
        this.starved = false;
        return;
      }
      if (event.data?.type === "state") {
        this.producer?.postMessage(event.data);
        return;
      }
      if (event.data?.type !== "connect") return;
      this.producer = event.data.port;
      this.producer.onmessage = (message) => {
        const packet = message.data;
        if (packet.epoch !== this.epoch) {
          this.recycle.buffer = this.transfer[0] = packet.buffer;
          this.producer.postMessage(this.recycle, this.transfer);
          return;
        }
        if (this.count === this.queue.length) {
          this.port.postMessage({ error: "Audio transport queue overflow" });
          return;
        }
        this.queue[this.tail] = {
          pcm: new Float32Array(packet.buffer),
          frames: packet.frames,
        };
        this.tail = (this.tail + 1) % this.queue.length;
        this.count++;
      };
    };
  }
  process(_, outputs) {
    const channels = outputs[0];
    if (!channels?.length) return true;
    const left = channels[0],
      right = channels[1];
    if (!this.started) {
      if (this.count < this.queue.length) return true;
      this.started = true;
    }
    let written = 0;
    while (written < left.length && this.count) {
      const packet = this.queue[this.head],
        n = Math.min(left.length - written, packet.frames - this.offset);
      for (let i = 0; i < n; i++) {
        left[written + i] = packet.pcm[this.offset + i];
        if (right)
          right[written + i] = packet.pcm[packet.frames + this.offset + i];
      }
      written += n;
      this.offset += n;
      if (this.offset === packet.frames) {
        this.recycle.buffer = this.transfer[0] = packet.pcm.buffer;
        this.producer.postMessage(this.recycle, this.transfer);
        this.queue[this.head] = null;
        this.head = (this.head + 1) % this.queue.length;
        this.count--;
        this.offset = 0;
      }
    }
    if (written < left.length) {
      this.underrunFrames += left.length - written;
      if (!this.starved) this.underruns++;
      this.starved = true;
    } else this.starved = false;
    this.frames += left.length;
    if (this.frames >= this.reportAt) {
      this.reportAt += sampleRate;
      this.port.postMessage({
        type: "metrics",
        underruns: this.underruns,
        underrunFrames: this.underrunFrames,
        queuedChunks: this.count,
      });
    }
    return true;
  }
}
registerProcessor("engine-pcm", PcmAudioProcessor);
