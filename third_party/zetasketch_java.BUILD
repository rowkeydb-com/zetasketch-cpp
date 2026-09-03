# Build file overlaid on the Java reference implementation's source
# archive. The published Maven artifact is the 2019 release, which is
# three years behind this source and enforces different precision
# limits, so the differential tests build the library themselves.

package(default_visibility = ["//visibility:public"])

licenses(["notice"])

proto_library(
    name = "zetasketch_proto",
    srcs = glob(["proto/*.proto"]),
    strip_import_prefix = "proto",
    deps = ["@protobuf//:descriptor_proto"],
)

java_proto_library(
    name = "zetasketch_java_proto",
    deps = [":zetasketch_proto"],
)

# The bias tables are generated from this file; a test compares the
# generated header against it.
filegroup(
    name = "data_java",
    srcs = ["java/com/google/zetasketch/internal/hllplus/Data.java"],
)

java_plugin(
    name = "auto_value_plugin",
    processor_class = "com.google.auto.value.processor.AutoValueProcessor",
    deps = ["@maven//:com_google_auto_value_auto_value"],
)

# The testing subpackage is excluded: it exists to support the
# reference implementation's own test suite and pulls in Truth, which
# nothing here needs.
java_library(
    name = "zetasketch",
    srcs = glob(
        ["java/**/*.java"],
        exclude = ["java/com/google/zetasketch/testing/**"],
    ),
    plugins = [":auto_value_plugin"],
    deps = [
        ":zetasketch_java_proto",
        "@maven//:com_google_auto_value_auto_value_annotations",
        "@maven//:com_google_code_findbugs_jsr305",
        "@maven//:com_google_errorprone_error_prone_annotations",
        "@maven//:com_google_guava_guava",
        "@maven//:com_google_protobuf_protobuf_java",
        "@maven//:it_unimi_dsi_fastutil",
        "@maven//:org_checkerframework_checker_qual",
    ],
)
