# Keep our own packages. Default Android ProGuard rules handle the rest.
-keep class de.jacksitlab.libremoto.** { *; }
# OsmAnd AIDL interface + Parcelable impls (generated Stub is reflection-free,
# but keep the parcelable CREATOR + field access so the IPC layer survives).
-keep class net.osmand.aidl.** { *; }
