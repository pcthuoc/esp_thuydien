Import("env")

# Override board flash size to 16MB (module uses N16R8 variant)
env.BoardConfig().update("upload.flash_size", "16MB")
env.BoardConfig().update("upload.maximum_size", 16777216)
env.BoardConfig().update("build.flash_size", "16MB")
