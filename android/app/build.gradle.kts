plugins {
    alias(libs.plugins.android.application)
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

dependencies {
    implementation(libs.androidx.appcompat)
    implementation(libs.androidx.core)
    implementation(libs.games.activity)
    implementation(libs.oboe)
}
