-keep class org.libsdl.app.** { *; }
-keep class org.vpinball.app.jni.** { *; }
# VR menu shell: libvrshell calls these static methods by name through JNI
-keep class org.vpinball.app.vr.VrShell { *; }

-keepclasseswithmembernames class * {
    native <methods>;
}

-keep class kotlinx.serialization.** { *; }
-keepclassmembers class * {
    @kotlinx.serialization.Serializable <fields>;
}

-keepattributes Signature
-keepattributes *Annotation*
-keepattributes InnerClasses
-keepattributes EnclosingMethod

-dontwarn javax.annotation.**
-dontwarn org.conscrypt.**
-dontwarn org.bouncycastle.**
-dontwarn org.openjsse.**
