cc_library(
    name = "sds",
    srcs = ["sds.c"],
    hdrs = [
        "sds.h",
        "sdsalloc.h",
    ],
    defines = select({
        "@platforms//os:windows": ["__attribute__(x)="],
        "//conditions:default": [],
    }),
    linkstatic = True,
    visibility = ["//visibility:public"],
)
