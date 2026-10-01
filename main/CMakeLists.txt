idf_component_register(
    SRCS "main.c"
         "repeater_lwip_hooks.c"
    INCLUDE_DIRS "."
    EMBED_FILES
        "login.html"
        "admin.html"
        "tools.html"
        "chat.html"
)
