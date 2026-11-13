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
        // Build stamp: git describe at build time — visible on the start
        // screen (BuildStamp.ID) so a stale install is instantly obvious.
        versionName = providers.exec {
            commandLine("git", "describe", "--always", "--dirty")
        }.standardOutput.asText.get().trim()
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

tasks.withType<Test>().configureEach {
    // The pbf parser test runs with the TABLET's heap budget (256 MB) to
    // prove the two-pass primitive-array parser doesn't OOM there.
    maxHeapSize = "256m"
    // Pass -DrunTour through to the test JVM (TourPreviewTest switch):
    //   ./gradlew testDebugUnitTest --tests '*TourPreview*' -DrunTour=true
    systemProperty("runTour", System.getProperty("runTour", "false"))
    // Viewer mode: explicit coordinates + zoom (TourPreviewTest):
    //   -DtourStops="52.51933,13.34229" -DtourScale=48
    systemProperty("tourStops", System.getProperty("tourStops", ""))
    systemProperty("tourScale", System.getProperty("tourScale", "48"))

    // GPX replay + GeoJSON export (map verification side tools):
    //   -DrunGpx=true -DgpxFile=… -DgpxMap=… [-DgpxStep=50]
    //   -DrunExport=true -DexportMap=… [-DexportPbf=…]
    systemProperty("runGpx", System.getProperty("runGpx", "false"))
    systemProperty("gpxFile", System.getProperty("gpxFile", ""))
    systemProperty("gpxMap", System.getProperty("gpxMap", ""))
    systemProperty("gpxStep", System.getProperty("gpxStep", "50"))
    systemProperty("runExport", System.getProperty("runExport", "false"))
    systemProperty("exportMap", System.getProperty("exportMap", ""))
    systemProperty("exportPbf", System.getProperty("exportPbf", ""))
    // .lmm builder helper:
    //   -DbuildPbf=… -DbuildOut=…
    systemProperty("buildPbf", System.getProperty("buildPbf", ""))
    systemProperty("buildOut", System.getProperty("buildOut", ""))
}

dependencies {
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("com.google.android.material:material:1.12.0")
    testImplementation("junit:junit:4.13.2")
}
