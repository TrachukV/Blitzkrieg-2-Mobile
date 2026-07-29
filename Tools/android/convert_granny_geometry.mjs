#!/usr/bin/env node

import { mkdir, readdir, readFile, writeFile } from "node:fs/promises";
import { basename, join, resolve } from "node:path";

import { parseModel } from "granny-ro-js";

const MAGIC = Buffer.from([0x42, 0x4b, 0x32, 0x4d, 0x53, 0x48, 0x31, 0x00]);
const FORMAT_VERSION = 2;
const VERTEX_FLOAT_COUNT = 8;

function usage() {
  process.stderr.write(
    "Usage: node convert_granny_geometry.mjs " +
      "--input <Data/bin/Geometries> --output <Converted/Geometries> " +
      "[--all | <resource-id> ...]\n",
  );
}

function parseArguments(argv) {
  let input = "";
  let output = "";
  let all = false;
  const ids = [];
  for (let index = 0; index < argv.length; ++index) {
    const argument = argv[index];
    if (argument === "--input") {
      input = argv[++index] ?? "";
    } else if (argument === "--output") {
      output = argv[++index] ?? "";
    } else if (argument === "--all") {
      all = true;
    } else if (/^\d+$/.test(argument)) {
      ids.push(argument);
    } else {
      throw new Error(`Unknown argument: ${argument}`);
    }
  }
  if (!input || !output || (!all && ids.length === 0)) {
    usage();
    process.exitCode = 2;
    return null;
  }
  return {
    input: resolve(input),
    output: resolve(output),
    all,
    ids,
  };
}

function toArrayBuffer(buffer) {
  return buffer.buffer.slice(
    buffer.byteOffset,
    buffer.byteOffset + buffer.byteLength,
  );
}

function orderedMeshes(parsed) {
  return parsed.meshes;
}

function serializeGeometry(parsed) {
  const meshes = orderedMeshes(parsed).filter(
    (mesh) =>
      mesh.vertexCount > 0 &&
      mesh.indexCount >= 3 &&
      mesh.positions.length === mesh.vertexCount,
  );
  if (meshes.length === 0) {
    throw new Error("resource has no renderable meshes");
  }

  let byteLength = MAGIC.length + 8;
  for (const mesh of meshes) {
    const groups =
      mesh.triangleGroups.length > 0
        ? mesh.triangleGroups
        : [{ materialIndex: 0, triFirst: 0, triCount: mesh.indexCount / 3 }];
    byteLength += 12;
    byteLength += mesh.vertexCount * VERTEX_FLOAT_COUNT * 4;
    byteLength += mesh.indexCount * 4;
    byteLength += groups.length * 12;
  }
  const output = Buffer.allocUnsafe(byteLength);
  MAGIC.copy(output, 0);
  output.writeUInt32LE(FORMAT_VERSION, 8);
  output.writeUInt32LE(meshes.length, 12);
  let offset = 16;

  for (const mesh of meshes) {
    const groups =
      mesh.triangleGroups.length > 0
        ? mesh.triangleGroups
        : [{ materialIndex: 0, triFirst: 0, triCount: mesh.indexCount / 3 }];
    output.writeUInt32LE(mesh.vertexCount, offset);
    output.writeUInt32LE(mesh.indexCount, offset + 4);
    output.writeUInt32LE(groups.length, offset + 8);
    offset += 12;
    for (let vertex = 0; vertex < mesh.vertexCount; ++vertex) {
      const position = mesh.positions[vertex] ?? [0, 0, 0];
      const normal = mesh.normals[vertex] ?? [0, 0, 1];
      const uv = mesh.uvs[vertex] ?? [0, 0];
      const values = [
        position[0] ?? 0,
        position[1] ?? 0,
        position[2] ?? 0,
        normal[0] ?? 0,
        normal[1] ?? 0,
        normal[2] ?? 1,
        uv[0] ?? 0,
        uv[1] ?? 0,
      ];
      for (const value of values) {
        output.writeFloatLE(Number.isFinite(value) ? value : 0, offset);
        offset += 4;
      }
    }
    for (const index of mesh.indices) {
      if (!Number.isInteger(index) || index < 0 || index >= mesh.vertexCount) {
        throw new Error(`mesh contains invalid vertex index ${index}`);
      }
      output.writeUInt32LE(index, offset);
      offset += 4;
    }
    for (const group of groups) {
      if (
        !Number.isInteger(group.materialIndex) ||
        group.materialIndex < 0 ||
        !Number.isInteger(group.triFirst) ||
        group.triFirst < 0 ||
        !Number.isInteger(group.triCount) ||
        group.triCount <= 0 ||
        (group.triFirst + group.triCount) * 3 > mesh.indexCount
      ) {
        throw new Error("mesh contains an invalid material triangle group");
      }
      output.writeUInt32LE(group.materialIndex, offset);
      output.writeUInt32LE(group.triFirst, offset + 4);
      output.writeUInt32LE(group.triCount, offset + 8);
      offset += 12;
    }
  }
  return { output, meshes };
}

async function resourceIds(options) {
  if (!options.all) {
    return [...new Set(options.ids)].sort((left, right) => Number(left) - Number(right));
  }
  const entries = await readdir(options.input, { withFileTypes: true });
  return entries
    .filter((entry) => entry.isFile() && /^\d+$/.test(entry.name))
    .map((entry) => entry.name)
    .sort((left, right) => Number(left) - Number(right));
}

async function convertOne(options, id) {
  const source = join(options.input, id);
  const target = join(options.output, `${id}.bk2mesh`);
  const sourceBytes = await readFile(source);
  // Geometry conversion does not need embedded Granny textures. Avoiding the
  // texture path also keeps unrelated legacy IGC payloads from blocking a mesh.
  const parsed = parseModel(toArrayBuffer(sourceBytes));
  const { output, meshes } = serializeGeometry(parsed);
  await writeFile(target, output);
  return {
    id,
    sourceBytes: sourceBytes.length,
    outputBytes: output.length,
    meshes: meshes.length,
    vertices: meshes.reduce((sum, mesh) => sum + mesh.vertexCount, 0),
    triangles: meshes.reduce((sum, mesh) => sum + mesh.indexCount / 3, 0),
  };
}

async function main() {
  const options = parseArguments(process.argv.slice(2));
  if (options === null) {
    return;
  }
  await mkdir(options.output, { recursive: true });
  const ids = await resourceIds(options);
  let converted = 0;
  let skipped = 0;
  let failed = 0;
  let vertices = 0;
  let triangles = 0;
  let outputBytes = 0;
  for (const id of ids) {
    try {
      const result = await convertOne(options, id);
      ++converted;
      vertices += result.vertices;
      triangles += result.triangles;
      outputBytes += result.outputBytes;
      if (!options.all) {
        process.stdout.write(
          `geometry=${result.id}; meshes=${result.meshes}; ` +
            `vertices=${result.vertices}; triangles=${result.triangles}; ` +
            `output=${basename(join(options.output, `${id}.bk2mesh`))}\n`,
        );
      }
    } catch (error) {
      if (error.message === "resource has no renderable meshes") {
        ++skipped;
      } else {
        ++failed;
        process.stderr.write(`geometry=${id}; error=${error.message}\n`);
      }
    }
  }
  process.stdout.write(
    `geometry_conversion_complete=1; requested=${ids.length}; ` +
      `converted=${converted}; skipped=${skipped}; failed=${failed}; vertices=${vertices}; ` +
      `triangles=${triangles}; output_bytes=${outputBytes}\n`,
  );
  if (failed > 0) {
    process.exitCode = 1;
  }
}

await main();
