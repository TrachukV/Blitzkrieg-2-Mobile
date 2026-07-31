#!/usr/bin/env node

import { readFile } from "node:fs/promises";

const MAGIC = "BK2MSH1\0";
const VERTEX_FLOAT_COUNT = 8;
const MAX_SKIN_INFLUENCES = 4;
const MATRIX_FLOAT_COUNT = 16;
const ERROR_TOLERANCE = 0.00001;

class Reader {
  constructor(buffer) {
    this.buffer = buffer;
    this.offset = 0;
  }

  ensure(bytes) {
    if (this.offset + bytes > this.buffer.length) {
      throw new Error(
        `unexpected end of cache at ${this.offset} + ${bytes}`,
      );
    }
  }

  skip(bytes) {
    this.ensure(bytes);
    this.offset += bytes;
  }

  uint16() {
    this.ensure(2);
    const value = this.buffer.readUInt16LE(this.offset);
    this.offset += 2;
    return value;
  }

  uint32() {
    this.ensure(4);
    const value = this.buffer.readUInt32LE(this.offset);
    this.offset += 4;
    return value;
  }

  int32() {
    this.ensure(4);
    const value = this.buffer.readInt32LE(this.offset);
    this.offset += 4;
    return value;
  }

  float32() {
    this.ensure(4);
    const value = this.buffer.readFloatLE(this.offset);
    this.offset += 4;
    return value;
  }

  vertex() {
    return Array.from(
      { length: VERTEX_FLOAT_COUNT },
      () => this.float32(),
    );
  }
}

function parseCache(buffer) {
  const reader = new Reader(buffer);
  reader.ensure(8);
  const magic = buffer.toString("latin1", 0, 8);
  reader.skip(8);
  if (magic !== MAGIC) {
    throw new Error(`unexpected cache magic ${JSON.stringify(magic)}`);
  }
  const version = reader.uint32();
  if (version < 3 || version > 5) {
    throw new Error(`unsupported cache version ${version}`);
  }
  const meshCount = reader.uint32();
  const parts = [];
  for (let mesh = 0; mesh < meshCount; ++mesh) {
    const vertexCount = reader.uint32();
    const indexCount = reader.uint32();
    const groupCount = reader.uint32();
    const frameCount = reader.uint32();
    const duration = reader.float32();
    const skinningMode = version >= 5 ? reader.uint32() : 0;
    const storedFrameCount = version >= 5 ? 1 : frameCount;
    const vertexFrames = [];
    for (let frame = 0; frame < storedFrameCount; ++frame) {
      vertexFrames.push(
        Array.from({ length: vertexCount }, () => reader.vertex()),
      );
    }
    reader.skip(indexCount * 4);
    reader.skip(groupCount * 12);

    let boneCount = 0;
    if (version >= 4) {
      boneCount = reader.uint32();
      for (let bone = 0; bone < boneCount; ++bone) {
        reader.int32();
        reader.skip(12);
        const nameLength = reader.uint32();
        reader.skip((nameLength + 3) & ~3);
      }
      reader.skip(vertexCount * 4);
    }

    const skinWeights = [];
    const skinningFrames = [];
    if (version >= 5 && skinningMode === 1) {
      for (let vertex = 0; vertex < vertexCount; ++vertex) {
        const bones = Array.from(
          { length: MAX_SKIN_INFLUENCES },
          () => reader.uint16(),
        );
        const weights = Array.from(
          { length: MAX_SKIN_INFLUENCES },
          () => reader.float32(),
        );
        skinWeights.push({ bones, weights });
      }
      for (let frame = 0; frame < frameCount; ++frame) {
        skinningFrames.push(
          Array.from(
            { length: boneCount },
            () =>
              Array.from(
                { length: MATRIX_FLOAT_COUNT },
                () => reader.float32(),
              ),
          ),
        );
      }
    }
    parts.push({
      vertexCount,
      frameCount,
      duration,
      vertexFrames,
      boneCount,
      skinWeights,
      skinningFrames,
    });
  }
  if (reader.offset !== buffer.length) {
    throw new Error(
      `cache has ${buffer.length - reader.offset} trailing bytes`,
    );
  }
  return { version, parts };
}

function skinVertex(source, skin, matrices) {
  const result = [...source];
  let x = 0;
  let y = 0;
  let z = 0;
  let nx = 0;
  let ny = 0;
  let nz = 0;
  let totalWeight = 0;
  for (let influence = 0; influence < MAX_SKIN_INFLUENCES; ++influence) {
    const weight = skin.weights[influence];
    const matrix = matrices[skin.bones[influence]];
    if (!(weight > 0) || !matrix) {
      continue;
    }
    x +=
      (matrix[0] * source[0] +
        matrix[4] * source[1] +
        matrix[8] * source[2] +
        matrix[12]) *
      weight;
    y +=
      (matrix[1] * source[0] +
        matrix[5] * source[1] +
        matrix[9] * source[2] +
        matrix[13]) *
      weight;
    z +=
      (matrix[2] * source[0] +
        matrix[6] * source[1] +
        matrix[10] * source[2] +
        matrix[14]) *
      weight;
    nx +=
      (matrix[0] * source[3] +
        matrix[4] * source[4] +
        matrix[8] * source[5]) *
      weight;
    ny +=
      (matrix[1] * source[3] +
        matrix[5] * source[4] +
        matrix[9] * source[5]) *
      weight;
    nz +=
      (matrix[2] * source[3] +
        matrix[6] * source[4] +
        matrix[10] * source[5]) *
      weight;
    totalWeight += weight;
  }
  if (totalWeight <= 1e-8) {
    return result;
  }
  result[0] = x / totalWeight;
  result[1] = y / totalWeight;
  result[2] = z / totalWeight;
  const normalLength = Math.hypot(nx, ny, nz);
  if (normalLength > 1e-8) {
    result[3] = nx / normalLength;
    result[4] = ny / normalLength;
    result[5] = nz / normalLength;
  } else {
    result[3] = 0;
    result[4] = 0;
    result[5] = 1;
  }
  return result;
}

function validate(legacy, compact) {
  if (legacy.version > 4 || compact.version !== 5) {
    throw new Error(
      `expected legacy version <= 4 and compact version 5, got ` +
        `${legacy.version} and ${compact.version}`,
    );
  }
  if (legacy.parts.length !== compact.parts.length) {
    throw new Error("cache part count changed");
  }
  let vertices = 0;
  let frames = 0;
  let maxPositionError = 0;
  let maxNormalError = 0;
  let maxUvError = 0;
  for (let partIndex = 0; partIndex < legacy.parts.length; ++partIndex) {
    const oldPart = legacy.parts[partIndex];
    const newPart = compact.parts[partIndex];
    if (
      oldPart.vertexCount !== newPart.vertexCount ||
      oldPart.frameCount !== newPart.frameCount ||
      Math.abs(oldPart.duration - newPart.duration) > ERROR_TOLERANCE
    ) {
      throw new Error(`part ${partIndex} animation metadata changed`);
    }
    vertices += oldPart.vertexCount;
    frames += oldPart.frameCount;
    for (let frame = 0; frame < oldPart.frameCount; ++frame) {
      for (let vertex = 0; vertex < oldPart.vertexCount; ++vertex) {
        const expected = oldPart.vertexFrames[frame][vertex];
        const actual =
          newPart.skinningFrames.length > 0
            ? skinVertex(
                newPart.vertexFrames[0][vertex],
                newPart.skinWeights[vertex],
                newPart.skinningFrames[frame],
              )
            : newPart.vertexFrames[0][vertex];
        for (let component = 0; component < 3; ++component) {
          maxPositionError = Math.max(
            maxPositionError,
            Math.abs(expected[component] - actual[component]),
          );
        }
        for (let component = 3; component < 6; ++component) {
          maxNormalError = Math.max(
            maxNormalError,
            Math.abs(expected[component] - actual[component]),
          );
        }
        for (let component = 6; component < 8; ++component) {
          maxUvError = Math.max(
            maxUvError,
            Math.abs(expected[component] - actual[component]),
          );
        }
      }
    }
  }
  if (
    maxPositionError > ERROR_TOLERANCE ||
    maxNormalError > ERROR_TOLERANCE ||
    maxUvError > ERROR_TOLERANCE
  ) {
    throw new Error(
      `runtime skinning differs from baked cache: position=` +
        `${maxPositionError}; normal=${maxNormalError}; uv=${maxUvError}`,
    );
  }
  return {
    parts: legacy.parts.length,
    vertices,
    frames,
    maxPositionError,
    maxNormalError,
    maxUvError,
  };
}

async function main() {
  const [legacyPath, compactPath] = process.argv.slice(2);
  if (!legacyPath || !compactPath) {
    throw new Error(
      "Usage: node validate_runtime_skinning.mjs " +
        "<legacy-v3-or-v4.bk2mesh> <compact-v5.bk2mesh>",
    );
  }
  const [legacyBytes, compactBytes] = await Promise.all([
    readFile(legacyPath),
    readFile(compactPath),
  ]);
  const result = validate(
    parseCache(legacyBytes),
    parseCache(compactBytes),
  );
  process.stdout.write(
    `runtime_skinning_validation=passed; parts=${result.parts}; ` +
      `vertices=${result.vertices}; frames=${result.frames}; ` +
      `max_position_error=${result.maxPositionError}; ` +
      `max_normal_error=${result.maxNormalError}; ` +
      `max_uv_error=${result.maxUvError}; ` +
      `legacy_bytes=${legacyBytes.length}; ` +
      `compact_bytes=${compactBytes.length}\n`,
  );
}

main().catch((error) => {
  process.stderr.write(`${error.stack ?? error}\n`);
  process.exitCode = 1;
});
