const { script, command, file } = require("./cli")
const Path = require("path")

script((argv) => {
  let packageName, packageVersion
  console.log(`npm development pack of '${argv.name}'...`)

  // Compute package reference
  packageName = argv.name
  switch (argv.config.toLowerCase()) {
    case "debug": packageVersion = `${argv.version}-debug`; break
    case "relwithdebinfo": packageVersion = `${argv.version}-release`; break
    case "release": packageVersion = `${argv.version}`; break
    default: throw new Error(`Cannot make package for build configuration '${argv.config}'`)
  }

  // Generated required files
  const packageJsonPath = Path.resolve(argv.source, "package.json")
  const previousPackageJson = file.read.json(packageJsonPath)
  file.write.json(packageJsonPath, {
    ...previousPackageJson,
    name: packageName,
    version: packageVersion,
  })

  // Pack and deliver to destination
  command.exec("npm pack", { cwd: argv.source })
  file.move.toDir(Path.resolve(argv.source, `${packageName}-${packageVersion}.tgz`), argv.destination)

  // Clean generated files
  if (previousPackageJson) file.write.json(packageJsonPath, previousPackageJson)
  else file.remove(packageJsonPath)
}, {
  arguments: {
    "name": {
      type: "string",
      require: true,
    },
    "version": {
      type: "string",
      require: true,
    },
    "config": {
      type: "string",
      require: true,
    },
    "source": {
      type: "string",
      require: true,
    },
    "destination": {
      type: "string",
      require: true,
    }
  }
})
