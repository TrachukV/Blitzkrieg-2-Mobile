#!/usr/bin/env node

import { mkdir, readdir, readFile, rm, writeFile } from "node:fs/promises";
import { basename, join, resolve } from "node:path";

import { parseAnimated, parseModel, poseSkeletonAt } from "granny-ro-js";

const MAGIC = Buffer.from([0x42, 0x4b, 0x32, 0x4d, 0x53, 0x48, 0x31, 0x00]);
const FORMAT_VERSION = 3;
const VERTEX_FLOAT_COUNT = 8;
const DEFAULT_ANIMATION_FRAME_COUNT = 16;
const MAX_MESH_COUNT = 128;
const UUID_PATTERN =
  /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;

function isResourceId(value) {
  return /^\d+$/.test(value) || UUID_PATTERN.test(value);
}

function runtimeGeometryId(value) {
  if (/^\d+$/.test(value)) {
    return Number.parseInt(value, 10);
  }
  let hash = 0x811c9dc5;
  for (const byte of Buffer.from(value.toUpperCase(), "ascii")) {
    hash ^= byte;
    hash = Math.imul(hash, 0x01000193) >>> 0;
  }
  return (0x40000000 | (hash & 0x3fffffff)) >>> 0;
}

function usage() {
  process.stderr.write(
    "Usage: node convert_granny_geometry.mjs " +
      "--input <Data/bin/Geometries> --output <Converted/Geometries> " +
      "[--idle-animation <Data/bin/Animations/resource>] " +
      "[--move-animation <Data/bin/Animations/resource>] " +
      "[--attack-animation <Data/bin/Animations/resource>] " +
      "[--death-animation <Data/bin/Animations/resource>] " +
      "[--animation-frames <count>] " +
      "[--skip-unsupported] " +
      "[--all | <resource-id> ...]\n",
  );
}

function parseArguments(argv) {
  let input = "";
  let output = "";
  let idleAnimation = "";
  let moveAnimation = "";
  let attackAnimation = "";
  let deathAnimation = "";
  let animationFrames = DEFAULT_ANIMATION_FRAME_COUNT;
  let all = false;
  let skipUnsupported = false;
  const ids = [];
  for (let index = 0; index < argv.length; ++index) {
    const argument = argv[index];
    if (argument === "--input") {
      input = argv[++index] ?? "";
    } else if (argument === "--output") {
      output = argv[++index] ?? "";
    } else if (argument === "--idle-animation") {
      idleAnimation = argv[++index] ?? "";
    } else if (argument === "--move-animation") {
      moveAnimation = argv[++index] ?? "";
    } else if (argument === "--attack-animation") {
      attackAnimation = argv[++index] ?? "";
    } else if (argument === "--death-animation") {
      deathAnimation = argv[++index] ?? "";
    } else if (argument === "--animation-frames") {
      animationFrames = Number.parseInt(argv[++index] ?? "", 10);
    } else if (argument === "--all") {
      all = true;
    } else if (argument === "--skip-unsupported") {
      skipUnsupported = true;
    } else if (isResourceId(argument)) {
      ids.push(argument);
    } else {
      throw new Error(`Unknown argument: ${argument}`);
    }
  }
  if (
    !input ||
    !output ||
    (!all && ids.length === 0) ||
    !Number.isInteger(animationFrames) ||
    animationFrames < 2 ||
    animationFrames > 120
  ) {
    usage();
    process.exitCode = 2;
    return null;
  }
  return {
    input: resolve(input),
    output: resolve(output),
    idleAnimation: idleAnimation ? resolve(idleAnimation) : "",
    moveAnimation: moveAnimation ? resolve(moveAnimation) : "",
    attackAnimation: attackAnimation ? resolve(attackAnimation) : "",
    deathAnimation: deathAnimation ? resolve(deathAnimation) : "",
    animationFrames,
    all,
    skipUnsupported,
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

function transformPoint(matrix, value, translation) {
  const x = value[0] ?? 0;
  const y = value[1] ?? 0;
  const z = value[2] ?? 0;
  return [
    matrix[0] * x + matrix[4] * y + matrix[8] * z +
      (translation ? matrix[12] : 0),
    matrix[1] * x + matrix[5] * y + matrix[9] * z +
      (translation ? matrix[13] : 0),
    matrix[2] * x + matrix[6] * y + matrix[10] * z +
      (translation ? matrix[14] : 0),
  ];
}

function normalize(value) {
  const length = Math.hypot(value[0], value[1], value[2]);
  return length > 1e-8
    ? [value[0] / length, value[1] / length, value[2] / length]
    : [0, 0, 1];
}

function canUseAnimation(mesh, skeleton, animation) {
  if (
    !skeleton ||
    !animation ||
    mesh.vertexWeights.length !== mesh.vertexCount ||
    mesh.boneBindings.length === 0 ||
    !mesh.vertexWeights.some((weights) => weights.length > 0)
  ) {
    return false;
  }
  const skeletonNames = new Set(skeleton.bones.map((bone) => bone.name));
  const trackNames = new Set(
    animation.trackGroups.flatMap((group) =>
      group.transformTracks.map((track) => track.name),
    ),
  );
  return mesh.boneBindings.every(
    (binding) =>
      skeletonNames.has(binding.name) && trackNames.has(binding.name),
  );
}

function animatedFrames(mesh, skeleton, animation, frameCount) {
  if (!canUseAnimation(mesh, skeleton, animation)) {
    return [];
  }
  const skeletonIndexByName = new Map(
    skeleton.bones.map((bone, index) => [bone.name, index]),
  );
  const bindingToSkeleton = mesh.boneBindings.map((binding) =>
    skeletonIndexByName.get(binding.name),
  );
  const frames = [];
  for (let frame = 0; frame < frameCount; ++frame) {
    const time = animation.duration * frame / frameCount;
    const pose = poseSkeletonAt(skeleton, animation, time);
    const positions = [];
    const normals = [];
    for (let vertex = 0; vertex < mesh.vertexCount; ++vertex) {
      const weights = mesh.vertexWeights[vertex];
      const sourcePosition = mesh.positions[vertex] ?? [0, 0, 0];
      const sourceNormal = mesh.normals[vertex] ?? [0, 0, 1];
      let position = [0, 0, 0];
      let normal = [0, 0, 0];
      let totalWeight = 0;
      for (const weight of weights) {
        const skeletonIndex = bindingToSkeleton[weight.boneIndex];
        if (
          !Number.isInteger(skeletonIndex) ||
          !pose.skinningMatrices[skeletonIndex] ||
          weight.weight <= 0
        ) {
          continue;
        }
        const matrix = pose.skinningMatrices[skeletonIndex];
        const weightedPosition = transformPoint(matrix, sourcePosition, true);
        const weightedNormal = transformPoint(matrix, sourceNormal, false);
        position[0] += weightedPosition[0] * weight.weight;
        position[1] += weightedPosition[1] * weight.weight;
        position[2] += weightedPosition[2] * weight.weight;
        normal[0] += weightedNormal[0] * weight.weight;
        normal[1] += weightedNormal[1] * weight.weight;
        normal[2] += weightedNormal[2] * weight.weight;
        totalWeight += weight.weight;
      }
      if (totalWeight <= 1e-8) {
        position = [...sourcePosition];
        normal = [...sourceNormal];
      } else if (Math.abs(totalWeight - 1) > 1e-5) {
        position = position.map((value) => value / totalWeight);
        normal = normal.map((value) => value / totalWeight);
      }
      positions.push(position);
      normals.push(normalize(normal));
    }
    frames.push({ positions, normals });
  }
  return frames;
}

function serializeGeometry(parsed, animation, animationFrameCount) {
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
    const frames = animatedFrames(
      mesh,
      parsed.skeletons[0],
      animation,
      animationFrameCount,
    );
    mesh.androidAnimationFrames = frames;
    const frameCount = frames.length > 0 ? frames.length : 1;
    byteLength += 20;
    byteLength += frameCount * mesh.vertexCount * VERTEX_FLOAT_COUNT * 4;
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
    const frames = mesh.androidAnimationFrames;
    const frameCount = frames.length > 0 ? frames.length : 1;
    output.writeUInt32LE(frameCount, offset + 12);
    output.writeFloatLE(
      frames.length > 0 ? animation.duration : 0,
      offset + 16,
    );
    offset += 20;
    for (let frame = 0; frame < frameCount; ++frame) {
      for (let vertex = 0; vertex < mesh.vertexCount; ++vertex) {
        const position =
          frames[frame]?.positions[vertex] ??
          mesh.positions[vertex] ??
          [0, 0, 0];
        const normal =
          frames[frame]?.normals[vertex] ??
          mesh.normals[vertex] ??
          [0, 0, 1];
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
  const ids = options.all
    ? (await readdir(options.input, { withFileTypes: true }))
        .filter((entry) => entry.isFile() && isResourceId(entry.name))
        .map((entry) => entry.name)
    : [...new Set(options.ids)];
  const runtimeIds = new Map();
  for (const id of ids) {
    const runtimeId = runtimeGeometryId(id);
    const previous = runtimeIds.get(runtimeId);
    if (previous !== undefined && previous !== id) {
      throw new Error(
        `Runtime geometry ID collision: ${previous} and ${id} -> ${runtimeId}`,
      );
    }
    runtimeIds.set(runtimeId, id);
  }
  return ids.sort((left, right) =>
    left.localeCompare(right, "en", { numeric: true }),
  );
}

async function writeAnimationVariant(
  parsed,
  animation,
  animationFrames,
  target,
) {
  if (animation === null) {
    await rm(target, { force: true });
    return { animatedMeshes: 0, outputBytes: 0 };
  }
  const converted = serializeGeometry(parsed, animation, animationFrames);
  const animatedMeshes = converted.meshes.filter(
    (mesh) => mesh.androidAnimationFrames.length > 0,
  ).length;
  if (animatedMeshes === 0) {
    await rm(target, { force: true });
    return { animatedMeshes: 0, outputBytes: 0 };
  }
  await writeFile(target, converted.output);
  return {
    animatedMeshes,
    outputBytes: converted.output.length,
  };
}

async function convertOne(
  options,
  id,
  idleAnimation,
  moveAnimation,
  attackAnimation,
  deathAnimation,
) {
  const source = join(options.input, id);
  const runtimeId = runtimeGeometryId(id);
  const target = join(options.output, `${runtimeId}.bk2mesh`);
  const sourceBytes = await readFile(source);
  // Geometry conversion does not need embedded Granny textures. Avoiding the
  // texture path also keeps unrelated legacy IGC payloads from blocking a mesh.
  // The decoder defaults to 32 meshes, but shipped bridges and large ships
  // contain up to 60. Match the Android cache reader's validated limit so
  // multi-part original models are not silently truncated.
  const parsed = parseModel(toArrayBuffer(sourceBytes), {
    maxMeshes: MAX_MESH_COUNT,
  });
  const primary = serializeGeometry(
    parsed,
    idleAnimation,
    options.animationFrames,
  );
  const animatedMeshes = primary.meshes.filter(
    (mesh) => mesh.androidAnimationFrames.length > 0,
  ).length;
  await writeFile(target, primary.output);

  const move = await writeAnimationVariant(
    parsed,
    moveAnimation,
    options.animationFrames,
    join(options.output, `${runtimeId}.move.bk2mesh`),
  );
  const attack = await writeAnimationVariant(
    parsed,
    attackAnimation,
    options.animationFrames,
    join(options.output, `${runtimeId}.attack.bk2mesh`),
  );
  const death = await writeAnimationVariant(
    parsed,
    deathAnimation,
    options.animationFrames,
    join(options.output, `${runtimeId}.death.bk2mesh`),
  );
  return {
    id,
    runtimeId,
    sourceBytes: sourceBytes.length,
    outputBytes: primary.output.length,
    moveOutputBytes: move.outputBytes,
    attackOutputBytes: attack.outputBytes,
    deathOutputBytes: death.outputBytes,
    meshes: primary.meshes.length,
    vertices: primary.meshes.reduce(
      (sum, mesh) => sum + mesh.vertexCount,
      0,
    ),
    triangles: primary.meshes.reduce(
      (sum, mesh) => sum + mesh.indexCount / 3,
      0,
    ),
    animatedMeshes,
    moveAnimatedMeshes: move.animatedMeshes,
    attackAnimatedMeshes: attack.animatedMeshes,
    deathAnimatedMeshes: death.animatedMeshes,
  };
}

async function main() {
  const options = parseArguments(process.argv.slice(2));
  if (options === null) {
    return;
  }
  await mkdir(options.output, { recursive: true });
  let idleAnimation = null;
  if (options.idleAnimation) {
    const animationBytes = await readFile(options.idleAnimation);
    idleAnimation =
      parseAnimated(toArrayBuffer(animationBytes)).animations[0] ?? null;
    if (idleAnimation === null) {
      throw new Error("idle animation resource has no animation");
    }
  }
  let moveAnimation = null;
  if (options.moveAnimation) {
    const animationBytes = await readFile(options.moveAnimation);
    moveAnimation =
      parseAnimated(toArrayBuffer(animationBytes)).animations[0] ?? null;
    if (moveAnimation === null) {
      throw new Error("move animation resource has no animation");
    }
  }
  let attackAnimation = null;
  if (options.attackAnimation) {
    const animationBytes = await readFile(options.attackAnimation);
    attackAnimation =
      parseAnimated(toArrayBuffer(animationBytes)).animations[0] ?? null;
    if (attackAnimation === null) {
      throw new Error("attack animation resource has no animation");
    }
  }
  let deathAnimation = null;
  if (options.deathAnimation) {
    const animationBytes = await readFile(options.deathAnimation);
    deathAnimation =
      parseAnimated(toArrayBuffer(animationBytes)).animations[0] ?? null;
    if (deathAnimation === null) {
      throw new Error("death animation resource has no animation");
    }
  }
  const ids = await resourceIds(options);
  let converted = 0;
  let skipped = 0;
  let blocked = 0;
  let failed = 0;
  let vertices = 0;
  let triangles = 0;
  let outputBytes = 0;
  let moveOutputBytes = 0;
  let attackOutputBytes = 0;
  let deathOutputBytes = 0;
  let animatedMeshes = 0;
  let moveAnimatedMeshes = 0;
  let attackAnimatedMeshes = 0;
  let deathAnimatedMeshes = 0;
  let moveCacheFiles = 0;
  let attackCacheFiles = 0;
  let deathCacheFiles = 0;
  for (const id of ids) {
    try {
      const result = await convertOne(
        options,
        id,
        idleAnimation,
        moveAnimation,
        attackAnimation,
        deathAnimation,
      );
      ++converted;
      vertices += result.vertices;
      triangles += result.triangles;
      outputBytes += result.outputBytes;
      moveOutputBytes += result.moveOutputBytes;
      attackOutputBytes += result.attackOutputBytes;
      deathOutputBytes += result.deathOutputBytes;
      animatedMeshes += result.animatedMeshes;
      moveAnimatedMeshes += result.moveAnimatedMeshes;
      attackAnimatedMeshes += result.attackAnimatedMeshes;
      deathAnimatedMeshes += result.deathAnimatedMeshes;
      moveCacheFiles += result.moveOutputBytes > 0 ? 1 : 0;
      attackCacheFiles += result.attackOutputBytes > 0 ? 1 : 0;
      deathCacheFiles += result.deathOutputBytes > 0 ? 1 : 0;
      if (!options.all) {
        process.stdout.write(
          `geometry=${result.id}; runtime_id=${result.runtimeId}; ` +
            `meshes=${result.meshes}; ` +
            `animated_meshes=${result.animatedMeshes}; ` +
            `move_animated_meshes=${result.moveAnimatedMeshes}; ` +
            `attack_animated_meshes=${result.attackAnimatedMeshes}; ` +
            `death_animated_meshes=${result.deathAnimatedMeshes}; ` +
            `vertices=${result.vertices}; triangles=${result.triangles}; ` +
            `output=${basename(join(options.output, `${result.runtimeId}.bk2mesh`))}; ` +
            `move_output=${result.moveOutputBytes > 0 ? basename(join(options.output, `${result.runtimeId}.move.bk2mesh`)) : "<none>"}; ` +
            `attack_output=${result.attackOutputBytes > 0 ? basename(join(options.output, `${result.runtimeId}.attack.bk2mesh`)) : "<none>"}; ` +
            `death_output=${result.deathOutputBytes > 0 ? basename(join(options.output, `${result.runtimeId}.death.bk2mesh`)) : "<none>"}\n`,
        );
      }
    } catch (error) {
      if (error.message === "resource has no renderable meshes") {
        ++skipped;
      } else if (
        options.skipUnsupported &&
        /^unsupported compression \d+$/.test(error.message)
      ) {
        ++blocked;
      } else {
        ++failed;
        process.stderr.write(`geometry=${id}; error=${error.message}\n`);
      }
    }
  }
  process.stdout.write(
    `geometry_conversion_complete=1; requested=${ids.length}; ` +
      `converted=${converted}; skipped=${skipped}; blocked=${blocked}; ` +
      `failed=${failed}; vertices=${vertices}; ` +
      `triangles=${triangles}; animated_meshes=${animatedMeshes}; ` +
      `move_animated_meshes=${moveAnimatedMeshes}; ` +
      `move_cache_files=${moveCacheFiles}; ` +
      `attack_animated_meshes=${attackAnimatedMeshes}; ` +
      `attack_cache_files=${attackCacheFiles}; ` +
      `death_animated_meshes=${deathAnimatedMeshes}; ` +
      `death_cache_files=${deathCacheFiles}; ` +
      `output_bytes=${outputBytes}; move_output_bytes=${moveOutputBytes}; ` +
      `attack_output_bytes=${attackOutputBytes}; ` +
      `death_output_bytes=${deathOutputBytes}\n`,
  );
  if (failed > 0) {
    process.exitCode = 1;
  }
}

await main();
