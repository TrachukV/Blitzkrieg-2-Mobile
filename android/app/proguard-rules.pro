# The native runtime reaches back into Java by name, so R8 must not rename or
# remove those entry points.

# GameActivity's own JNI entry point and the fields native_app_glue reads.
-keep class com.google.androidgamesdk.GameActivity { *; }
-keep class androidx.games.** { *; }

# bk2_android_video_bridge.cpp resolves this class through FindClass and its
# playback entry points through GetStaticMethodID.
-keep class com.nival.blitzkrieg2.NativeBridge { *; }

# Everything the native library declares with RegisterNatives or the
# Java_... naming convention.
-keepclasseswithmembernames class * {
    native <methods>;
}

# The Activities are named in AndroidManifest.xml and started by name from
# each other.
-keep class com.nival.blitzkrieg2.Blitzkrieg2Activity { *; }
-keep class com.nival.blitzkrieg2.MissionSelectActivity { *; }
-keep class com.nival.blitzkrieg2.VideoPlayerActivity { *; }

# Oboe ships its callbacks through JNI as well.
-keep class com.google.oboe.** { *; }
