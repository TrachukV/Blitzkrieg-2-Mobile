#!/usr/bin/env node

import { mkdir, readdir, readFile, rm, writeFile } from "node:fs/promises";
import { basename, join, resolve } from "node:path";

import { parseAnimated, parseModel, poseSkeletonAt } from "granny-ro-js";

const MAGIC = Buffer.from([0x42, 0x4b, 0x32, 0x4d, 0x53, 0x48, 0x31, 0x00]);
const FORMAT_VERSION = 4;
const VERTEX_FLOAT_COUNT = 8;
const DEFAULT_ANIMATION_FRAME_COUNT = 16;
const MAX_MESH_COUNT = 128;
const UUID_PATTERN =
  /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
const PROCEDURAL_GEOMETRY_BUILDERS = new Map([
  [
    "8E1CF9C4-6B9E-4930-90A2-2B92D658005E",
    buildGermanAssaultBoat,
  ],
  [
    "80058E1C-6B9E-4930-90A2-2B92D658005E",
    buildSovietArmoredSeaHunter,
  ],
]);

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
      "[--lying-idle-animation <Data/bin/Animations/resource>] " +
      "[--lying-move-animation <Data/bin/Animations/resource>] " +
      "[--lying-attack-animation <Data/bin/Animations/resource>] " +
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
  let lyingIdleAnimation = "";
  let lyingMoveAnimation = "";
  let lyingAttackAnimation = "";
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
    } else if (argument === "--lying-idle-animation") {
      lyingIdleAnimation = argv[++index] ?? "";
    } else if (argument === "--lying-move-animation") {
      lyingMoveAnimation = argv[++index] ?? "";
    } else if (argument === "--lying-attack-animation") {
      lyingAttackAnimation = argv[++index] ?? "";
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
    lyingIdleAnimation: lyingIdleAnimation
      ? resolve(lyingIdleAnimation)
      : "",
    lyingMoveAnimation: lyingMoveAnimation
      ? resolve(lyingMoveAnimation)
      : "",
    lyingAttackAnimation: lyingAttackAnimation
      ? resolve(lyingAttackAnimation)
      : "",
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

// The shipped German assault boat is one of the old Oodle1-compressed Granny
// resources that the open decoder cannot read. Keep its original dimensions
// and material instead of substituting an unrelated vehicle or drawing the
// runtime debug marker. This modest reconstruction is a narrow wooden assault
// boat with a flared hull, gunwales, floor, benches and motor.
function buildGermanAssaultBoat() {
  const positions = [];
  const normals = [];
  const uvs = [];
  const indices = [];

  const addQuad = (corners, uvRect) => {
    const edgeA = corners[1].map((value, index) => value - corners[0][index]);
    const edgeB = corners[2].map((value, index) => value - corners[0][index]);
    const normal = normalize([
      edgeA[1] * edgeB[2] - edgeA[2] * edgeB[1],
      edgeA[2] * edgeB[0] - edgeA[0] * edgeB[2],
      edgeA[0] * edgeB[1] - edgeA[1] * edgeB[0],
    ]);
    const base = positions.length;
    const [u0, v0, u1, v1] = uvRect;
    const faceUvs = [
      [u0, v1],
      [u1, v1],
      [u1, v0],
      [u0, v0],
    ];
    for (let corner = 0; corner < 4; ++corner) {
      positions.push(corners[corner]);
      normals.push(normal);
      uvs.push(faceUvs[corner]);
    }
    // Keep the open interior readable with the original one-sided material
    // from any gameplay camera angle.
    indices.push(
      base,
      base + 1,
      base + 2,
      base,
      base + 2,
      base + 3,
      base,
      base + 2,
      base + 1,
      base,
      base + 3,
      base + 2,
    );
  };

  const addBox = (minX, maxX, minY, maxY, minZ, maxZ, uvRect) => {
    addQuad(
      [
        [minX, minY, maxZ],
        [maxX, minY, maxZ],
        [maxX, maxY, maxZ],
        [minX, maxY, maxZ],
      ],
      uvRect,
    );
    addQuad(
      [
        [minX, minY, minZ],
        [minX, maxY, minZ],
        [maxX, maxY, minZ],
        [maxX, minY, minZ],
      ],
      uvRect,
    );
    addQuad(
      [
        [minX, minY, minZ],
        [maxX, minY, minZ],
        [maxX, minY, maxZ],
        [minX, minY, maxZ],
      ],
      uvRect,
    );
    addQuad(
      [
        [maxX, minY, minZ],
        [maxX, maxY, minZ],
        [maxX, maxY, maxZ],
        [maxX, minY, maxZ],
      ],
      uvRect,
    );
    addQuad(
      [
        [maxX, maxY, minZ],
        [minX, maxY, minZ],
        [minX, maxY, maxZ],
        [maxX, maxY, maxZ],
      ],
      uvRect,
    );
    addQuad(
      [
        [minX, maxY, minZ],
        [minX, minY, minZ],
        [minX, minY, maxZ],
        [minX, maxY, maxZ],
      ],
      uvRect,
    );
  };

  const centerY = -1.46791;
  const sections = [
    { y: centerY - 4.355685, width: 0.54 },
    { y: centerY - 3.45, width: 0.69 },
    { y: centerY - 0.15, width: 0.73 },
    { y: centerY + 3.25, width: 0.55 },
    { y: centerY + 4.355685, width: 0.04 },
  ];
  const hullUv = [0.24, 0.38, 0.79, 0.72];
  for (let section = 0; section + 1 < sections.length; ++section) {
    const current = sections[section];
    const next = sections[section + 1];
    for (const side of [-1, 1]) {
      addQuad(
        [
          [side * current.width, current.y, 0.84],
          [side * current.width * 0.58, current.y, -0.08],
          [side * next.width * 0.58, next.y, -0.08],
          [side * next.width, next.y, 0.84],
        ],
        hullUv,
      );
    }
    addQuad(
      [
        [-current.width * 0.58, current.y, -0.08],
        [current.width * 0.58, current.y, -0.08],
        [next.width * 0.58, next.y, -0.08],
        [-next.width * 0.58, next.y, -0.08],
      ],
      [0.27, 0.53, 0.76, 0.70],
    );
  }

  addQuad(
    [
      [-0.54, centerY - 3.55, 0.31],
      [0.54, centerY - 3.55, 0.31],
      [0.43, centerY + 3.05, 0.31],
      [-0.43, centerY + 3.05, 0.31],
    ],
    [0.30, 0.43, 0.73, 0.66],
  );

  const timberUv = [0.31, 0.11, 0.72, 0.24];
  for (const section of sections.slice(0, -1)) {
    addBox(
      -section.width,
      -Math.max(0.0, section.width - 0.09),
      section.y - 0.38,
      section.y + 0.38,
      0.78,
      0.88,
      timberUv,
    );
    addBox(
      Math.max(0.0, section.width - 0.09),
      section.width,
      section.y - 0.38,
      section.y + 0.38,
      0.78,
      0.88,
      timberUv,
    );
  }
  for (const benchY of [
    centerY - 2.55,
    centerY - 1.25,
    centerY + 0.05,
    centerY + 1.35,
    centerY + 2.55,
  ]) {
    addBox(
      -0.58,
      0.58,
      benchY - 0.09,
      benchY + 0.09,
      0.61,
      0.72,
      timberUv,
    );
  }

  const metalUv = [0.83, 0.32, 0.96, 0.65];
  addBox(
    -0.24,
    0.24,
    centerY - 4.32,
    centerY - 3.88,
    0.34,
    1.11,
    metalUv,
  );
  addBox(
    -0.08,
    0.08,
    centerY - 4.62,
    centerY - 4.18,
    -0.01,
    0.39,
    metalUv,
  );

  return {
    meshes: [
      {
        vertexCount: positions.length,
        indexCount: indices.length,
        positions,
        normals,
        uvs,
        indices,
        triangleGroups: [
          {
            materialIndex: 0,
            triFirst: 0,
            triCount: indices.length / 3,
          },
        ],
        vertexWeights: Array.from(
          { length: positions.length },
          () => [],
        ),
        boneBindings: [],
      },
    ],
    skeletons: [],
  };
}

// The Soviet armored sea hunter is another Oodle1-only naval resource. Build
// a recognizable low-poly patrol craft at the original 4.23444 x 24.9885
// footprint, using its shipped texture atlas: narrow steel hull, raised deck,
// bridge, fore/aft gun tubs, funnel and mast.
function buildSovietArmoredSeaHunter() {
  const positions = [];
  const normals = [];
  const uvs = [];
  const indices = [];

  const addQuad = (corners, uvRect) => {
    const edgeA = corners[1].map((value, index) => value - corners[0][index]);
    const edgeB = corners[2].map((value, index) => value - corners[0][index]);
    const normal = normalize([
      edgeA[1] * edgeB[2] - edgeA[2] * edgeB[1],
      edgeA[2] * edgeB[0] - edgeA[0] * edgeB[2],
      edgeA[0] * edgeB[1] - edgeA[1] * edgeB[0],
    ]);
    const base = positions.length;
    const [u0, v0, u1, v1] = uvRect;
    const faceUvs = [
      [u0, v1],
      [u1, v1],
      [u1, v0],
      [u0, v0],
    ];
    for (let corner = 0; corner < 4; ++corner) {
      positions.push(corners[corner]);
      normals.push(normal);
      uvs.push(faceUvs[corner]);
    }
    indices.push(
      base,
      base + 1,
      base + 2,
      base,
      base + 2,
      base + 3,
      base,
      base + 2,
      base + 1,
      base,
      base + 3,
      base + 2,
    );
  };

  const addBox = (minX, maxX, minY, maxY, minZ, maxZ, uvRect) => {
    addQuad(
      [
        [minX, minY, maxZ],
        [maxX, minY, maxZ],
        [maxX, maxY, maxZ],
        [minX, maxY, maxZ],
      ],
      uvRect,
    );
    addQuad(
      [
        [minX, minY, minZ],
        [minX, maxY, minZ],
        [maxX, maxY, minZ],
        [maxX, minY, minZ],
      ],
      uvRect,
    );
    addQuad(
      [
        [minX, minY, minZ],
        [maxX, minY, minZ],
        [maxX, minY, maxZ],
        [minX, minY, maxZ],
      ],
      uvRect,
    );
    addQuad(
      [
        [maxX, minY, minZ],
        [maxX, maxY, minZ],
        [maxX, maxY, maxZ],
        [maxX, minY, maxZ],
      ],
      uvRect,
    );
    addQuad(
      [
        [maxX, maxY, minZ],
        [minX, maxY, minZ],
        [minX, maxY, maxZ],
        [maxX, maxY, maxZ],
      ],
      uvRect,
    );
    addQuad(
      [
        [minX, maxY, minZ],
        [minX, minY, minZ],
        [minX, minY, maxZ],
        [minX, maxY, maxZ],
      ],
      uvRect,
    );
  };

  const addPrism = (centerX, centerY, radius, minZ, maxZ, sides, uvRect) => {
    const lower = [];
    const upper = [];
    for (let side = 0; side < sides; ++side) {
      const angle = side * Math.PI * 2 / sides;
      lower.push([
        centerX + Math.cos(angle) * radius,
        centerY + Math.sin(angle) * radius,
        minZ,
      ]);
      upper.push([
        centerX + Math.cos(angle) * radius,
        centerY + Math.sin(angle) * radius,
        maxZ,
      ]);
    }
    for (let side = 0; side < sides; ++side) {
      const next = (side + 1) % sides;
      addQuad(
        [lower[side], lower[next], upper[next], upper[side]],
        uvRect,
      );
      addQuad(
        [
          [centerX, centerY, maxZ],
          upper[side],
          upper[next],
          [centerX, centerY, maxZ],
        ],
        uvRect,
      );
    }
  };

  const deckUv = [0.01, 0.25, 0.64, 0.47];
  const hullUv = [0.01, 0.49, 0.65, 0.74];
  const bottomUv = [0.01, 0.79, 0.65, 0.95];
  const steelUv = [0.68, 0.77, 0.82, 0.97];
  const darkSteelUv = [0.73, 0.26, 0.95, 0.46];
  const sections = [
    { y: -12.49425, width: 0.45 },
    { y: -10.8, width: 1.65 },
    { y: -7.0, width: 2.03 },
    { y: 5.8, width: 2.11 },
    { y: 9.8, width: 1.55 },
    { y: 12.49425, width: 0.05 },
  ];
  for (let section = 0; section + 1 < sections.length; ++section) {
    const current = sections[section];
    const next = sections[section + 1];
    for (const side of [-1, 1]) {
      addQuad(
        [
          [side * current.width, current.y, 1.62],
          [side * current.width * 0.72, current.y, -0.08],
          [side * next.width * 0.72, next.y, -0.08],
          [side * next.width, next.y, 1.62],
        ],
        hullUv,
      );
    }
    addQuad(
      [
        [-current.width * 0.72, current.y, -0.08],
        [current.width * 0.72, current.y, -0.08],
        [next.width * 0.72, next.y, -0.08],
        [-next.width * 0.72, next.y, -0.08],
      ],
      bottomUv,
    );
    addQuad(
      [
        [-current.width, current.y, 1.62],
        [current.width, current.y, 1.62],
        [next.width, next.y, 1.62],
        [-next.width, next.y, 1.62],
      ],
      deckUv,
    );
  }

  addBox(-1.58, 1.58, -4.7, 2.4, 1.58, 2.75, steelUv);
  addBox(-1.18, 1.18, 1.2, 5.2, 2.70, 4.05, darkSteelUv);
  addBox(-0.86, 0.86, 3.1, 5.35, 4.02, 4.68, steelUv);
  addBox(-0.58, 0.58, -6.3, -4.75, 2.72, 4.15, darkSteelUv);

  for (const gunY of [-8.25, 8.25]) {
    addPrism(0.0, gunY, 1.05, 1.60, 2.32, 8, steelUv);
    addBox(
      -0.16,
      0.16,
      gunY + 0.15,
      gunY + 3.05,
      2.17,
      2.43,
      darkSteelUv,
    );
  }

  addBox(-0.10, 0.10, 3.55, 3.75, 4.60, 8.48, darkSteelUv);
  addBox(-1.08, 1.08, 3.59, 3.71, 7.20, 7.34, darkSteelUv);
  addBox(-0.05, 0.05, 3.68, 4.75, 7.25, 7.36, darkSteelUv);
  addBox(-0.48, 0.48, 3.62, 4.55, 4.64, 4.76, steelUv);

  return {
    meshes: [
      {
        vertexCount: positions.length,
        indexCount: indices.length,
        positions,
        normals,
        uvs,
        indices,
        triangleGroups: [
          {
            materialIndex: 0,
            triFirst: 0,
            triCount: indices.length / 3,
          },
        ],
        vertexWeights: Array.from(
          { length: positions.length },
          () => [],
        ),
        boneBindings: [],
      },
    ],
    skeletons: [],
  };
}

function canUseAnimation(mesh, skeleton, animation) {
  const hasExplicitWeights =
    mesh.vertexWeights.some((weights) => weights.length > 0);
  // Large static animated models such as bridges store one rigid bone binding
  // per mesh and omit redundant per-vertex weights. Granny treats every
  // vertex in that mesh as fully bound to the sole binding.
  const hasRigidBinding = mesh.boneBindings.length === 1;
  if (
    !skeleton ||
    !animation ||
    mesh.boneBindings.length === 0 ||
    (hasExplicitWeights &&
      mesh.vertexWeights.length !== mesh.vertexCount) ||
    (!hasExplicitWeights && !hasRigidBinding)
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


// Bind-pose world pivot of a bone. The parser exposes the inverse bind matrix,
// so the pivot is -(R^T . t) of that inverse for the orthonormal rotations
// these skeletons use.
function bonePivot(bone) {
  const m = bone.inverseWorldTransform;
  if (!m || m.length < 16) {
    return [0, 0, 0];
  }
  const t = [m[12], m[13], m[14]];
  return [
    -(m[0] * t[0] + m[1] * t[1] + m[2] * t[2]),
    -(m[4] * t[0] + m[5] * t[1] + m[6] * t[2]),
    -(m[8] * t[0] + m[9] * t[1] + m[10] * t[2]),
  ];
}

// Index of the skeleton bone that carries most of a vertex's weight, which is
// what the runtime needs to rotate a turret or gun subtree.
function dominantBones(mesh, skeleton) {
  const none = 0xffffffff;
  const result = new Array(mesh.vertexCount).fill(none);
  if (!skeleton || !mesh.boneBindings || mesh.boneBindings.length === 0) {
    return result;
  }
  const skeletonIndexByName = new Map(
    skeleton.bones.map((bone, index) => [bone.name, index]),
  );
  const bindingToSkeleton = mesh.boneBindings.map((binding) =>
    skeletonIndexByName.get(binding.name),
  );
  // Vehicle parts are rigidly attached: one binding, no per-vertex weights.
  // The whole mesh then belongs to that single bone.
  const rigid =
    mesh.boneBindings.length === 1 &&
    !(mesh.vertexWeights ?? []).some((weights) => weights && weights.length > 0)
      ? bindingToSkeleton[0]
      : undefined;
  if (Number.isInteger(rigid)) {
    return result.fill(rigid);
  }
  for (let vertex = 0; vertex < mesh.vertexCount; ++vertex) {
    const weights = mesh.vertexWeights[vertex] ?? [];
    let best = none;
    let bestWeight = 0;
    for (const weight of weights) {
      const skeletonIndex = bindingToSkeleton[weight.boneIndex];
      if (!Number.isInteger(skeletonIndex) || weight.weight <= bestWeight) {
        continue;
      }
      best = skeletonIndex;
      bestWeight = weight.weight;
    }
    result[vertex] = best;
  }
  return result;
}

function boneTableBytes(skeleton) {
  if (!skeleton) {
    return 4;
  }
  let bytes = 4;
  for (const bone of skeleton.bones) {
    const name = Buffer.byteLength(bone.name ?? "", "utf8");
    bytes += 4 + 12 + 4 + ((name + 3) & ~3);
  }
  return bytes;
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
      const explicitWeights = mesh.vertexWeights[vertex] ?? [];
      const weights =
        explicitWeights.length > 0
          ? explicitWeights
          : mesh.boneBindings.length === 1
            ? [{ boneIndex: 0, weight: 1 }]
            : [];
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
    byteLength += boneTableBytes(parsed.skeletons[0]);
    byteLength += mesh.vertexCount * 4;
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

    const skeleton = parsed.skeletons[0];
    const bones = skeleton ? skeleton.bones : [];
    output.writeUInt32LE(bones.length, offset);
    offset += 4;
    for (const bone of bones) {
      output.writeInt32LE(
        Number.isInteger(bone.parentIndex) ? bone.parentIndex : -1,
        offset,
      );
      const pivot = bonePivot(bone);
      output.writeFloatLE(Number.isFinite(pivot[0]) ? pivot[0] : 0, offset + 4);
      output.writeFloatLE(Number.isFinite(pivot[1]) ? pivot[1] : 0, offset + 8);
      output.writeFloatLE(
        Number.isFinite(pivot[2]) ? pivot[2] : 0,
        offset + 12,
      );
      const name = Buffer.from(bone.name ?? "", "utf8");
      const padded = (name.length + 3) & ~3;
      output.writeUInt32LE(name.length, offset + 16);
      offset += 20;
      name.copy(output, offset);
      output.fill(0, offset + name.length, offset + padded);
      offset += padded;
    }
    const dominant = dominantBones(mesh, skeleton);
    for (let vertex = 0; vertex < mesh.vertexCount; ++vertex) {
      output.writeUInt32LE(dominant[vertex] >>> 0, offset);
      offset += 4;
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
  lyingIdleAnimation,
  lyingMoveAnimation,
  lyingAttackAnimation,
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
  const proceduralBuilder = PROCEDURAL_GEOMETRY_BUILDERS.get(id.toUpperCase());
  const parsed = proceduralBuilder
    ? proceduralBuilder()
    : parseModel(toArrayBuffer(sourceBytes), {
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
  const lyingIdle = await writeAnimationVariant(
    parsed,
    lyingIdleAnimation,
    options.animationFrames,
    join(options.output, `${runtimeId}.lying.bk2mesh`),
  );
  const lyingMove = await writeAnimationVariant(
    parsed,
    lyingMoveAnimation,
    options.animationFrames,
    join(options.output, `${runtimeId}.lying.move.bk2mesh`),
  );
  const lyingAttack = await writeAnimationVariant(
    parsed,
    lyingAttackAnimation,
    options.animationFrames,
    join(options.output, `${runtimeId}.lying.attack.bk2mesh`),
  );
  return {
    id,
    runtimeId,
    sourceBytes: sourceBytes.length,
    outputBytes: primary.output.length,
    moveOutputBytes: move.outputBytes,
    attackOutputBytes: attack.outputBytes,
    deathOutputBytes: death.outputBytes,
    lyingIdleOutputBytes: lyingIdle.outputBytes,
    lyingMoveOutputBytes: lyingMove.outputBytes,
    lyingAttackOutputBytes: lyingAttack.outputBytes,
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
    lyingIdleAnimatedMeshes: lyingIdle.animatedMeshes,
    lyingMoveAnimatedMeshes: lyingMove.animatedMeshes,
    lyingAttackAnimatedMeshes: lyingAttack.animatedMeshes,
    procedural: proceduralBuilder !== undefined,
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
  let lyingIdleAnimation = null;
  if (options.lyingIdleAnimation) {
    const animationBytes = await readFile(options.lyingIdleAnimation);
    lyingIdleAnimation =
      parseAnimated(toArrayBuffer(animationBytes)).animations[0] ?? null;
    if (lyingIdleAnimation === null) {
      throw new Error("lying idle animation resource has no animation");
    }
  }
  let lyingMoveAnimation = null;
  if (options.lyingMoveAnimation) {
    const animationBytes = await readFile(options.lyingMoveAnimation);
    lyingMoveAnimation =
      parseAnimated(toArrayBuffer(animationBytes)).animations[0] ?? null;
    if (lyingMoveAnimation === null) {
      throw new Error("lying move animation resource has no animation");
    }
  }
  let lyingAttackAnimation = null;
  if (options.lyingAttackAnimation) {
    const animationBytes = await readFile(options.lyingAttackAnimation);
    lyingAttackAnimation =
      parseAnimated(toArrayBuffer(animationBytes)).animations[0] ?? null;
    if (lyingAttackAnimation === null) {
      throw new Error("lying attack animation resource has no animation");
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
  let lyingIdleOutputBytes = 0;
  let lyingMoveOutputBytes = 0;
  let lyingAttackOutputBytes = 0;
  let animatedMeshes = 0;
  let moveAnimatedMeshes = 0;
  let attackAnimatedMeshes = 0;
  let deathAnimatedMeshes = 0;
  let lyingIdleAnimatedMeshes = 0;
  let lyingMoveAnimatedMeshes = 0;
  let lyingAttackAnimatedMeshes = 0;
  let moveCacheFiles = 0;
  let attackCacheFiles = 0;
  let deathCacheFiles = 0;
  let lyingIdleCacheFiles = 0;
  let lyingMoveCacheFiles = 0;
  let lyingAttackCacheFiles = 0;
  for (const id of ids) {
    try {
      const result = await convertOne(
        options,
        id,
        idleAnimation,
        moveAnimation,
        attackAnimation,
        deathAnimation,
        lyingIdleAnimation,
        lyingMoveAnimation,
        lyingAttackAnimation,
      );
      ++converted;
      vertices += result.vertices;
      triangles += result.triangles;
      outputBytes += result.outputBytes;
      moveOutputBytes += result.moveOutputBytes;
      attackOutputBytes += result.attackOutputBytes;
      deathOutputBytes += result.deathOutputBytes;
      lyingIdleOutputBytes += result.lyingIdleOutputBytes;
      lyingMoveOutputBytes += result.lyingMoveOutputBytes;
      lyingAttackOutputBytes += result.lyingAttackOutputBytes;
      animatedMeshes += result.animatedMeshes;
      moveAnimatedMeshes += result.moveAnimatedMeshes;
      attackAnimatedMeshes += result.attackAnimatedMeshes;
      deathAnimatedMeshes += result.deathAnimatedMeshes;
      lyingIdleAnimatedMeshes += result.lyingIdleAnimatedMeshes;
      lyingMoveAnimatedMeshes += result.lyingMoveAnimatedMeshes;
      lyingAttackAnimatedMeshes += result.lyingAttackAnimatedMeshes;
      moveCacheFiles += result.moveOutputBytes > 0 ? 1 : 0;
      attackCacheFiles += result.attackOutputBytes > 0 ? 1 : 0;
      deathCacheFiles += result.deathOutputBytes > 0 ? 1 : 0;
      lyingIdleCacheFiles += result.lyingIdleOutputBytes > 0 ? 1 : 0;
      lyingMoveCacheFiles += result.lyingMoveOutputBytes > 0 ? 1 : 0;
      lyingAttackCacheFiles += result.lyingAttackOutputBytes > 0 ? 1 : 0;
      if (!options.all) {
        process.stdout.write(
          `geometry=${result.id}; runtime_id=${result.runtimeId}; ` +
            `procedural=${result.procedural ? 1 : 0}; ` +
            `meshes=${result.meshes}; ` +
            `animated_meshes=${result.animatedMeshes}; ` +
            `move_animated_meshes=${result.moveAnimatedMeshes}; ` +
            `attack_animated_meshes=${result.attackAnimatedMeshes}; ` +
            `death_animated_meshes=${result.deathAnimatedMeshes}; ` +
            `lying_idle_animated_meshes=${result.lyingIdleAnimatedMeshes}; ` +
            `lying_move_animated_meshes=${result.lyingMoveAnimatedMeshes}; ` +
            `lying_attack_animated_meshes=${result.lyingAttackAnimatedMeshes}; ` +
            `vertices=${result.vertices}; triangles=${result.triangles}; ` +
            `output=${basename(join(options.output, `${result.runtimeId}.bk2mesh`))}; ` +
            `move_output=${result.moveOutputBytes > 0 ? basename(join(options.output, `${result.runtimeId}.move.bk2mesh`)) : "<none>"}; ` +
            `attack_output=${result.attackOutputBytes > 0 ? basename(join(options.output, `${result.runtimeId}.attack.bk2mesh`)) : "<none>"}; ` +
            `death_output=${result.deathOutputBytes > 0 ? basename(join(options.output, `${result.runtimeId}.death.bk2mesh`)) : "<none>"}; ` +
            `lying_idle_output=${result.lyingIdleOutputBytes > 0 ? basename(join(options.output, `${result.runtimeId}.lying.bk2mesh`)) : "<none>"}; ` +
            `lying_move_output=${result.lyingMoveOutputBytes > 0 ? basename(join(options.output, `${result.runtimeId}.lying.move.bk2mesh`)) : "<none>"}; ` +
            `lying_attack_output=${result.lyingAttackOutputBytes > 0 ? basename(join(options.output, `${result.runtimeId}.lying.attack.bk2mesh`)) : "<none>"}\n`,
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
      `lying_idle_animated_meshes=${lyingIdleAnimatedMeshes}; ` +
      `lying_idle_cache_files=${lyingIdleCacheFiles}; ` +
      `lying_move_animated_meshes=${lyingMoveAnimatedMeshes}; ` +
      `lying_move_cache_files=${lyingMoveCacheFiles}; ` +
      `lying_attack_animated_meshes=${lyingAttackAnimatedMeshes}; ` +
      `lying_attack_cache_files=${lyingAttackCacheFiles}; ` +
      `output_bytes=${outputBytes}; move_output_bytes=${moveOutputBytes}; ` +
      `attack_output_bytes=${attackOutputBytes}; ` +
      `death_output_bytes=${deathOutputBytes}; ` +
      `lying_idle_output_bytes=${lyingIdleOutputBytes}; ` +
      `lying_move_output_bytes=${lyingMoveOutputBytes}; ` +
      `lying_attack_output_bytes=${lyingAttackOutputBytes}\n`,
  );
  if (failed > 0) {
    process.exitCode = 1;
  }
}

await main();
