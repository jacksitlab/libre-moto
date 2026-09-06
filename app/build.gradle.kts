/* LibreMoto companion app — build config.
 * Kotlin, classic XML layouts, minSdk 29 (Android 10).
 * OsmAnd nav data via the official AIDL API (net.osmand.aidl, vendor copy
 * in src/main/java — see app/OSMAND_API_LICENSE.md). */
plugins {
    id("com.android.application") version "9.2.1"
    id("org.jetbrains.kotlin.android") version "2.2.10"
}

android {
    namespace = "de.jacksitlab.libremoto"
    compileSdk = 34

    defaultConfig {
        applicationId = "de.jacksitlab.libremoto"
        minSdk = 29
        targetSdk = 34
        versionCode = 1
        versionName = "0.1.0"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
        isCoreLibraryDesugaringEnabled = false
    }
    kotlinOptions { jvmTarget = "17" }
    buildFeatures { aidl = true }
    lint { abortOnError = false }
    testOptions { unitTests.isReturnDefaultValues = true }
}

dependencies {
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("com.google.android.material:material:1.12.0")
    testImplementation("junit:junit:4.13.2")
}
