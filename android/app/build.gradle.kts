import java.util.Properties

plugins {
    alias(libs.plugins.android.application)
}

// Release signing comes from outside the repository: keystore.properties
// beside this module, or the same four values in the environment for a CI
// runner. Without them the release variant still assembles, unsigned, so a
// contributor who only wants an optimised build is not blocked on secrets.
val releaseKeystoreProperties = Properties().apply {
    val file = rootProject.file("keystore.properties")
    if (file.isFile) {
        file.inputStream().use { load(it) }
    }
}

fun releaseSigningValue(key: String, environmentKey: String): String? =
    releaseKeystoreProperties.getProperty(key)
        ?: System.getenv(environmentKey)

val releaseKeystorePath =
    releaseSigningValue("storeFile", "BK2_KEYSTORE")
val releaseKeystoreFile =
    releaseKeystorePath?.let { rootProject.file(it) }
val hasReleaseSigning = releaseKeystoreFile?.isFile == true &&
        releaseSigningValue("storePassword", "BK2_KEYSTORE_PASSWORD") != null &&
        releaseSigningValue("keyAlias", "BK2_KEY_ALIAS") != null &&
        releaseSigningValue("keyPassword", "BK2_KEY_PASSWORD") != null

val generatedOriginalHudAssets =
    layout.buildDirectory.dir("generated/originalHudAssets")
val originalHudRoot = rootProject.file("../Complete/UI")
val originalChapterArrowRoot =
    rootProject.file("../Versions/Current/Data/UI/chaptermap/arrows")
val requiredOriginalHudFiles = listOf(
    originalHudRoot.resolve("Panels/MissionMain.tga"),
    originalHudRoot.resolve("MiniMap/foreground.tga"),
    originalHudRoot.resolve("New_mission/Middle_Panels_screen001.tga"),
    originalHudRoot.resolve("Buttons/Icons/UnitIconBackground.tga"),
    originalHudRoot.resolve("Buttons/Icons/Allies/Soldier.tga"),
    originalHudRoot.resolve("Buttons/Icons/Tank.tga"),
    originalHudRoot.resolve("Buttons/HitBars/GreenUnitBar.tga"),
    originalHudRoot.resolve("Buttons/HitBars/YellowUnitBar.tga"),
    originalHudRoot.resolve("Buttons/HitBars/RedUnitBar.tga"),
    originalHudRoot.resolve(
        "New_mission/Panels2/EscMenuBtn_003.tga"
    ),
    originalHudRoot.resolve(
        "New_mission/Panels2/ObjectivesBtn_002.tga"
    ),
    originalChapterArrowRoot.resolve("arrow_own.dds"),
    originalChapterArrowRoot.resolve("arrow_enemy.dds"),
    originalChapterArrowRoot.resolve("defence_own.dds"),
    originalChapterArrowRoot.resolve("defence_enemy.dds")
)
val stageOriginalHudAssets by tasks.registering(Sync::class) {
    into(generatedOriginalHudAssets)
    inputs.files(requiredOriginalHudFiles)
    inputs.dir(originalHudRoot.resolve("New_mission/ActionButtons"))
    doFirst {
        val missing = requiredOriginalHudFiles.filterNot { it.isFile }
        check(missing.isEmpty()) {
            "Missing required original HUD assets: " +
                    missing.joinToString()
        }
        check(
            originalHudRoot
                .resolve("New_mission/ActionButtons")
                .isDirectory
        ) {
            "Missing original HUD action-button directory"
        }
    }
    from(originalHudRoot.resolve("Panels/MissionMain.tga")) {
        into("Complete/UI/Panels")
    }
    from(originalHudRoot.resolve("MiniMap/foreground.tga")) {
        into("Complete/UI/MiniMap")
    }
    from(
        originalHudRoot.resolve(
            "New_mission/Middle_Panels_screen001.tga"
        )
    ) {
        into("Complete/UI/New_mission")
    }
    from(originalHudRoot.resolve("New_mission/ActionButtons")) {
        include("**/*.tga")
        into("Complete/UI/New_mission/ActionButtons")
    }
    from(originalHudRoot.resolve("New_mission/Panels2")) {
        include(
            "EscMenuBtn_003.tga",
            "ObjectivesBtn_002.tga"
        )
        into("Complete/UI/New_mission/Panels2")
    }
    from(originalHudRoot.resolve("Buttons/Icons")) {
        include("**/*.tga")
        into("Complete/UI/Buttons/Icons")
    }
    from(originalHudRoot.resolve("Buttons/HitBars")) {
        include(
            "GreenUnitBar.tga",
            "YellowUnitBar.tga",
            "RedUnitBar.tga"
        )
        into("Complete/UI/Buttons/HitBars")
    }
    from(originalChapterArrowRoot) {
        include(
            "arrow_own.dds",
            "arrow_enemy.dds",
            "defence_own.dds",
            "defence_enemy.dds"
        )
        into("UI/chaptermap/arrows")
    }
}

android {
    namespace = "com.nival.blitzkrieg2"
    compileSdk = 36

    defaultConfig {
        applicationId = "com.nival.blitzkrieg2"
        minSdk = 24
        targetSdk = 36
        versionCode = 2
        versionName = "0.9.0"

        ndk {
            abiFilters += listOf("arm64-v8a")
        }

        externalNativeBuild {
            cmake {
                // The debug variant is what gets played during the port, and
                // the engine is far too heavy to run unoptimised: without
                // this the CMake debug build compiles at -O0 and the mission
                // renderer spends the whole frame budget in vector loops.
                cFlags += listOf("-O2")
                cppFlags += listOf(
                    "-std=c++20",
                    "-fno-exceptions",
                    "-fms-extensions",
                    "-fdelayed-template-parsing",
                    "-O2"
                )
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    "-DBK2_ENABLE_LEGACY_SOURCES=OFF"
                )
            }
        }
    }

    signingConfigs {
        if (hasReleaseSigning) {
            create("release") {
                storeFile = releaseKeystoreFile
                storePassword =
                    releaseSigningValue("storePassword", "BK2_KEYSTORE_PASSWORD")
                keyAlias =
                    releaseSigningValue("keyAlias", "BK2_KEY_ALIAS")
                keyPassword =
                    releaseSigningValue("keyPassword", "BK2_KEY_PASSWORD")
            }
        }
    }

    buildTypes {
        getByName("debug") {
            // The engine is far too heavy to run unoptimised, so the debug
            // variant that gets played during the port keeps -O2 from
            // defaultConfig and only carries the extra native asserts.
            isJniDebuggable = true
        }
        getByName("release") {
            isMinifyEnabled = true
            isShrinkResources = true
            isDebuggable = false
            isJniDebuggable = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                file("proguard-rules.pro")
            )
            if (hasReleaseSigning) {
                signingConfig = signingConfigs.getByName("release")
            }
            ndk {
                // Play needs a symbol table to symbolicate native crashes;
                // the stripped library still ships in the APK.
                debugSymbolLevel = "SYMBOL_TABLE"
            }
            externalNativeBuild {
                cmake {
                    // NDEBUG also compiles out the port's debug-only key
                    // handlers and bgfx debug annotations.
                    cppFlags += listOf("-DNDEBUG")
                    cFlags += listOf("-DNDEBUG")
                }
            }
        }
    }

    buildFeatures {
        prefab = true
    }

    sourceSets {
        getByName("main").assets.directories.add(
            generatedOriginalHudAssets
                .get()
                .asFile
                .absolutePath
        )
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    packaging {
        resources.excludes += setOf(
            "META-INF/AL2.0",
            "META-INF/LGPL2.1"
        )
    }
}

tasks.named("preBuild").configure {
    dependsOn(stageOriginalHudAssets)
}

dependencies {
    implementation(libs.androidx.appcompat)
    implementation(libs.androidx.core)
    implementation(libs.games.activity)
    implementation(libs.oboe)
}
