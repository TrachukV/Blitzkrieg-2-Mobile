plugins {
    alias(libs.plugins.android.application)
}

val generatedOriginalHudAssets =
    layout.buildDirectory.dir("generated/originalHudAssets")
val originalHudRoot = rootProject.file("../Complete/UI")
val requiredOriginalHudFiles = listOf(
    originalHudRoot.resolve("Panels/MissionMain.tga"),
    originalHudRoot.resolve("MiniMap/foreground.tga"),
    originalHudRoot.resolve("New_mission/Middle_Panels_screen001.tga"),
    originalHudRoot.resolve("Buttons/Icons/UnitIconBackground.tga"),
    originalHudRoot.resolve("Buttons/Icons/Allies/Soldier.tga"),
    originalHudRoot.resolve("Buttons/Icons/Tank.tga"),
    originalHudRoot.resolve("Buttons/HitBars/GreenUnitBar.tga"),
    originalHudRoot.resolve("Buttons/HitBars/YellowUnitBar.tga"),
    originalHudRoot.resolve("Buttons/HitBars/RedUnitBar.tga")
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
}

android {
    namespace = "com.nival.blitzkrieg2"
    compileSdk = 36

    defaultConfig {
        applicationId = "com.nival.blitzkrieg2"
        minSdk = 24
        targetSdk = 36
        versionCode = 1
        versionName = "0.1-port-bootstrap"

        ndk {
            abiFilters += listOf("arm64-v8a")
        }

        externalNativeBuild {
            cmake {
                cppFlags += listOf(
                    "-std=c++20",
                    "-fno-exceptions",
                    "-fms-extensions",
                    "-fdelayed-template-parsing"
                )
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    "-DBK2_ENABLE_LEGACY_SOURCES=OFF"
                )
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
