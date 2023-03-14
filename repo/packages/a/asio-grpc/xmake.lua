package("asio-grpc")
    set_homepage("https://github.com/Tradias/asio-grpc")
    set_description("")
    set_license("")

    add_urls("https://github.com/Tradias/asio-grpc/archive/refs/tags/$(version).tar.gz",
             "https://github.com/Tradias/asio-grpc.git")
    add_versions("v2.5.0", "e4d8cfab235a3b2510807dfd9fab04da4538df76b9feb0e7398295186b520dbc")

    add_deps("cmake")
    add_deps("asio")

    on_install("linux", function (package)
        local configs = {}
        table.insert(configs, "-DCMAKE_BUILD_TYPE=Release")
        import("package.tools.cmake").install(package, configs, {buildir = os.tmpfile() .. ".dir"})
    end)
package_end()