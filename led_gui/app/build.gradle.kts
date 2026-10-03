plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.bastet.ledgui"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.bastet.ledgui"
        minSdk = 29
        targetSdk = 34
        versionCode = 7
        versionName = "1.6"
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            signingConfig = signingConfigs.getByName("debug")
        }
    }
}

base {
    archivesName.set("led_gui")
}

// The GUI is classic Views only (see UiKit.kt): no Compose code exists in
// this module, so the Compose plugin, BOM and artifacts would only slow the
// build and hide the real 120Hz ScrollView work that is already here.
dependencies {
    implementation("androidx.core:core-ktx:1.12.0")          // NotificationManagerCompat
    implementation("androidx.activity:activity:1.9.3")        // ComponentActivity
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.9.0")
}