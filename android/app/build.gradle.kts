// Release signing.
//
// The keystore comes from ANDROID_KEYSTORE_* environment variables (CI) or a
// key.properties beside this project (a developer machine). No .jks is ever
// committed; both sources are gitignored.
//
// When neither supplies one, release falls back to the debug keystore with a
// warning rather than failing the build -- otherwise an ordinary local build of
// a clean checkout would refuse to run. Such a build is fine to install by hand
// and will not be accepted by Play Console, which is the correct outcome.
import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

data class KeystoreConfig(
    val path: String,
    val storePassword: String,
    val keyAlias: String,
    val keyPassword: String,
)

fun resolveKeystore(): KeystoreConfig? {
    val env = System.getenv("ANDROID_KEYSTORE_PATH")
    val envStorePw = System.getenv("ANDROID_KEYSTORE_PASSWORD")
    val envAlias = System.getenv("ANDROID_KEY_ALIAS")
    val envKeyPw = System.getenv("ANDROID_KEY_PASSWORD")
    if (env != null && envStorePw != null && envAlias != null && envKeyPw != null) {
        logger.lifecycle("release: using keystore from ANDROID_KEYSTORE_PATH")
        return KeystoreConfig(env, envStorePw, envAlias, envKeyPw)
    }

    val propsFile = rootProject.file("key.properties")
    if (propsFile.exists()) {
        val p = Properties().apply { load(propsFile.inputStream()) }
        val path = p["storeFile"] as String?
        val storePw = p["storePassword"] as String?
        val alias = p["keyAlias"] as String?
        val keyPw = p["keyPassword"] as String?
        if (path != null && storePw != null && alias != null && keyPw != null) {
            logger.lifecycle("release: using keystore from ${propsFile.absolutePath}")
            return KeystoreConfig(path, storePw, alias, keyPw)
        }
    }

    logger.warn(
        "release: no ANDROID_KEYSTORE_* env vars and no key.properties; " +
            "falling back to the debug keystore. This build will not be " +
            "accepted by Play Console."
    )
    return null
}

val keystoreConfig = resolveKeystore()

android {
    namespace = "com.crownpark.retro_towns"
    compileSdk = 36
    // Pinned rather than floating: which NDK a build machine happens to have
    // should not decide what ships.  It is also the NDK the core was
    // cross-compiled with by android/build-core.sh, and the two must agree.
    ndkVersion = "28.2.13676358"

    defaultConfig {
        applicationId = "com.crownpark.retro_towns"
        // 28 is a floor, not a preference: it is the API the Tsugaru core
        // configures against, and a front end built for a newer API than the
        // libraries it loads fails at run time rather than at build time.
        minSdk = 28
        // Play refuses updates to an app targeting more than a year behind the
        // latest release.
        targetSdk = 36
        versionCode = 1
        versionName = "0.1.0"
        ndk {
            // Only the ABI the core has actually been built for. Listing more
            // ships an APK that installs and then fails to load a library,
            // which is worse than not shipping that ABI.
            abiFilters += "arm64-v8a"
        }
    }

    // The libraries are prebuilt by android/build-core.sh, not compiled by
    // Gradle; they just get packaged.
    sourceSets["main"].jniLibs.srcDirs("src/main/jniLibs")

    packaging {
        jniLibs {
            // Tsugaru is a large static core, and uncompressed libraries
            // install faster and are what extractNativeLibs=true promises.
            useLegacyPackaging = true
        }
    }

    signingConfigs {
        if (keystoreConfig != null) {
            create("release") {
                storeFile = file(keystoreConfig.path)
                storePassword = keystoreConfig.storePassword
                keyAlias = keystoreConfig.keyAlias
                keyPassword = keystoreConfig.keyPassword
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = if (keystoreConfig != null) {
                signingConfigs.getByName("release")
            } else {
                signingConfigs.getByName("debug")
            }
        }
        debug { isMinifyEnabled = false }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
}

dependencies {
    // SDL3's own Java glue, produced by the SDL3 Android build.  Nothing else:
    // this app ships no artwork client and no SAF picker, so it needs no
    // okhttp, no curl and no documentfile, and an APK that asks for no network
    // is a simpler thing to explain.
    implementation(files("libs/SDL3.jar"))
    implementation("androidx.documentfile:documentfile:1.0.0")
}
