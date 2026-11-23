#!/usr/bin/env bash
#
# Локальный прогон всех проверок, которые выполняет CI.
#
# Зачем: CI на GitHub падал дважды на том, что локально «проходило».
# Причина — разные версии инструментов (Ubuntu ставит clang-format 18,
# у разработчика на macOS 21) и пустой список файлов до первого коммита.
#
# Здесь версии зафиксированы так же, как в .github/workflows/ci.yml.
#
# Использование:
#   scripts/check.sh            # все проверки, хост-инструменты
#   scripts/check.sh format     # только форматирование
#   scripts/check.sh tests      # только юнит-тесты
#   scripts/check.sh build      # только сборка прошивки
#   scripts/check.sh docker     # всё в контейнере ubuntu:24.04 (как в CI)
#   scripts/check.sh all        # то же, что без аргументов

set -euo pipefail

# --- Версии, синхронные с CI ------------------------------------------------
CLANG_FORMAT_VERSION="21.1.8"
CPPCHECK_VERSION="2.13"
# Тег контейнера = версии ubuntu-latest на GitHub Actions
DOCKER_IMAGE="ubuntu:24.04"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

ok()   { printf '\033[32m✓\033[0m %s\n' "$*"; }
fail() { printf '\033[31m✗\033[0m %s\n' "$*" >&2; }
step() { printf '\n\033[1m== %s ==\033[0m\n' "$*"; }

# --- Список файлов ----------------------------------------------------------
# git ls-files возвращает пусто до первого коммита — тогда берём find.
collect_cpp_files() {
    local files
    files=$(git ls-files '*.cpp' '*.h' '*.hpp' 2>/dev/null || true)
    if [ -z "$files" ]; then
        files=$(find include src test -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) 2>/dev/null || true)
    fi
    printf '%s\n' "$files"
}

# --- Поиск clang-format нужной версии ---------------------------------------
# Порядок: venv проекта → pip-установка в .cache → системный (с проверкой).
find_clang_format() {
    local bin="$REPO_ROOT/.cache/tools/bin/clang-format"

    if [ -x "$bin" ] && "$bin" --version 2>/dev/null | grep -q "$CLANG_FORMAT_VERSION"; then
        printf '%s' "$bin"
        return 0
    fi

    # Ставим в .cache, чтобы не трогать системный Python и не требовать root.
    local venv="$REPO_ROOT/.cache/tools"
    if [ ! -x "$venv/bin/python" ]; then
        python3 -m venv "$venv" >/dev/null 2>&1
    fi
    if "$venv/bin/pip" install -q "clang-format==${CLANG_FORMAT_VERSION}" >/dev/null 2>&1; then
        printf '%s' "$bin"
        return 0
    fi

    return 1
}

# --- Проверки ---------------------------------------------------------------

check_format() {
    step "Форматирование (clang-format ${CLANG_FORMAT_VERSION})"

    local files cf
    files=$(collect_cpp_files)
    if [ -z "$files" ]; then
        fail "Не найдено ни одного файла C++ — проверка ничего не проверила"
        return 1
    fi

    local count
    count=$(printf '%s\n' "$files" | wc -l | tr -d ' ')

    if ! cf=$(find_clang_format); then
        fail "clang-format ${CLANG_FORMAT_VERSION} не установился"
        printf '  Поставь вручную: pip install clang-format==%s\n' "$CLANG_FORMAT_VERSION"
        return 1
    fi

    if "$cf" --dry-run --Werror $files 2>&1; then
        ok "форматирование: ${count} файлов"
    else
        fail "форматирование: нарушения найдены"
        printf '  Исправить: %s -i $(git ls-files "*.cpp" "*.h" "*.hpp")\n' "$cf"
        return 1
    fi
}

check_tests() {
    step "Юнит-тесты (env:native)"

    if ! command -v pio >/dev/null 2>&1; then
        fail "PlatformIO (pio) не найден"
        return 1
    fi

    local out
    if out=$(pio test -e native 2>&1); then
        printf '%s\n' "$out" | tail -8
        ok "тесты native"
    else
        printf '%s\n' "$out" | tail -40
        fail "тесты native: есть падения"
        return 1
    fi
}

check_cppcheck() {
    step "cppcheck"

    if ! command -v cppcheck >/dev/null 2>&1; then
        printf '\033[33m…\033[0m cppcheck не установлен — пропускаем (в CI он есть)\n'
        printf '  Поставить: brew install cppcheck\n'
        return 0
    fi

    local log
    log=$(mktemp)
    if cppcheck --enable=warning,performance,portability \
                 --error-exitcode=1 \
                 --inline-suppr \
                 --suppress=missingIncludeSystem \
                 -I include src/ 2>&1 | tee "$log"; then
        ok "cppcheck"
        rm -f "$log"
    else
        fail "cppcheck: ошибки"
        rm -f "$log"
        return 1
    fi
}

check_build() {
    step "Сборка прошивки (env:tab5)"

    if ! command -v pio >/dev/null 2>&1; then
        fail "PlatformIO (pio) не найден"
        return 1
    fi

    local out
    if out=$(pio run -e tab5 2>&1); then
        printf '%s\n' "$out" | grep -E "^RAM:|^Flash:|SUCCESS" || true
        ok "прошивка собирается"
    else
        printf '%s\n' "$out" | tail -40
        fail "сборка прошивки"
        return 1
    fi
}

# --- Прогон в Docker: максимально близко к CI -------------------------------
run_docker() {
    step "Все проверки в ${DOCKER_IMAGE}"

    if ! command -v docker >/dev/null 2>&1; then
        fail "Docker не найден"
        return 1
    fi
    if ! docker info >/dev/null 2>&1; then
        fail "Docker-демон не запущен"
        return 1
    fi

    docker run --rm -v "$REPO_ROOT":/w -w /w "$DOCKER_IMAGE" bash -c "
        set -e
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -qq >/dev/null
        apt-get install -y -qq python3 python3-pip python3-venv g++ make cppcheck git >/dev/null

        python3 -m venv /opt/venv
        /opt/venv/bin/pip install -q --upgrade platformio 'clang-format==${CLANG_FORMAT_VERSION}'

        export PATH=/opt/venv/bin:\$PATH

        echo '--- clang-format ---'
        clang-format --version
        files=\$(git ls-files '*.cpp' '*.h' '*.hpp')
        [ -n \"\$files\" ] || { echo 'нет файлов C++'; exit 1; }
        clang-format --dry-run --Werror \$files
        echo 'форматирование: OK'

        echo '--- cppcheck ---'
        cppcheck --enable=warning,performance,portability --error-exitcode=1 \
                 --inline-suppr --suppress=missingIncludeSystem -I include src/
        echo 'cppcheck: OK'

        echo '--- тесты ---'
        pio test -e native
    "
    ok "все проверки в контейнере прошли"
}

# --- main -------------------------------------------------------------------
target="${1:-all}"

case "$target" in
    format)   check_format ;;
    tests)    check_tests ;;
    cppcheck) check_cppcheck ;;
    build)    check_build ;;
    docker)   run_docker ;;
    all)      check_format; check_tests; check_cppcheck; check_build ;;
    *)
        printf 'Неизвестная цель: %s\n' "$target" >&2
        printf 'Цели: format, tests, cppcheck, build, docker, all\n' >&2
        exit 1
        ;;
esac

printf '\n\033[32mГотово.\033[0m\n'
