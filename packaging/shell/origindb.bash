#!/bin/bash
# Bash completion for OriginDB CLI

_origindb() {
    local cur prev opts
    COMPREPLY=()
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"

    # Main commands
    local commands="init server module database logs status exec generate help version"

    # Server subcommands
    local server_commands="start stop restart status logs"

    # Module subcommands
    local module_commands="init build deploy list remove"

    # Database subcommands
    local database_commands="create drop list backup restore"

    # Languages for init and generate
    local languages="csharp rust javascript go cpp"

    # Templates for init
    local templates="unity-game console-app web-api realtime-app"

    case "${prev}" in
        origindb)
            COMPREPLY=($(compgen -W "${commands}" -- ${cur}))
            return 0
            ;;
        server)
            COMPREPLY=($(compgen -W "${server_commands}" -- ${cur}))
            return 0
            ;;
        module)
            COMPREPLY=($(compgen -W "${module_commands}" -- ${cur}))
            return 0
            ;;
        database)
            COMPREPLY=($(compgen -W "${database_commands}" -- ${cur}))
            return 0
            ;;
        --lang|--language)
            COMPREPLY=($(compgen -W "${languages}" -- ${cur}))
            return 0
            ;;
        --template)
            COMPREPLY=($(compgen -W "${templates}" -- ${cur}))
            return 0
            ;;
        --port|-p)
            COMPREPLY=($(compgen -W "8080 9090 3000" -- ${cur}))
            return 0
            ;;
        --log-level|-l)
            COMPREPLY=($(compgen -W "trace debug info warn error" -- ${cur}))
            return 0
            ;;
        --output|-o)
            COMPREPLY=($(compgen -d -- ${cur}))
            return 0
            ;;
    esac

    # Handle flags
    local opts="--help -h --version -v --verbose --quiet --config -c"

    case "${COMP_WORDS[1]}" in
        init)
            opts="${opts} --lang --template --name"
            ;;
        server)
            case "${COMP_WORDS[2]}" in
                start)
                    opts="${opts} --port -p --grpc-port --data-dir --log-level -l --daemon --dev"
                    ;;
                stop|restart)
                    opts="${opts} --force"
                    ;;
            esac
            ;;
        module)
            case "${COMP_WORDS[2]}" in
                init)
                    opts="${opts} --lang --template"
                    ;;
                build)
                    opts="${opts} --release --parallel -j"
                    ;;
                deploy)
                    opts="${opts} --version --force"
                    ;;
            esac
            ;;
        database)
            case "${COMP_WORDS[2]}" in
                create|drop)
                    opts="${opts} --force"
                    ;;
                backup|restore)
                    opts="${opts} --file -f --compress"
                    ;;
            esac
            ;;
        logs)
            opts="${opts} --follow -f --lines -n --level"
            ;;
        exec)
            opts="${opts} --sql --input --module --reducer --args"
            ;;
        generate)
            opts="${opts} --lang --output -o --namespace"
            ;;
    esac

    COMPREPLY=($(compgen -W "${opts}" -- ${cur}))
    return 0
}

# Register the completion function
complete -F _origindb origindb