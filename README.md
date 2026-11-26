# MapCraft

Фанатская некоммерческая глобальная стратегия в сеттинге Майнкрафта: основа — EU4 (провинции, страны, войны),
экономика — упрощённая EU3, карта воксельная, UI и звуки — Майнкрафта. Игра для Windows `mapcraft.exe`, редактор
провинций `mapcraft_editor.exe` и симулятор без окна `mapcraft_sim.exe`.

## Сборка и запуск
Нужны MSVC Build Tools (`build.cmd` находит компилятор через vswhere), vcpkg (переменная `VCPKG_ROOT`) и
Minecraft 26.2 у игрока: без его jar игра и редактор не стартуют.
```bash
code/build.cmd                                  # сборка всего в code/build/release
code/build/release/mapcraft.exe                 # игра
code/build/release/mapcraft_editor.exe          # редактор провинций, пишет data/setup
code/build/release/mapcraft_editor.exe --check  # только проверить data/ (ошибки — в mapcraft_editor.log)
code/build/release/mapcraft_sim.exe scenarios/base.txt build/runs/base  # сценарий без окна дважды, бит в бит
```
Логи `mapcraft.log` и `mapcraft_editor.log` лежат рядом с exe. Управление — docs/CONTROLS.md.

## Ассеты
Текстуры, шрифт, звуки и модели Mojang **не распространяются** и в репозиторий не попадают: игра читает их из
установленного Майнкрафта игрока (jar версии; звуки — по индексу ассетов `.minecraft`, IDEAS.md, «Звук»). Шрифты
`fonts/blockyfont.otf` (названия на карте) и `fonts/ChakraPetch-*.ttf` (UI) в git не идут, как и другие бинарники.

## Документы
| Что | Где |
|---|---|
| План: срезы и ближайшие шаги, идеи | docs/ROADMAP.md, `NEXT.md`, `IDEAS.md` |
| Код по папкам, отладочное `DevOptions`, известные долги | docs/CODE.md |
| Симуляция: числа, раскладка данных, детерминизм, headless | docs/SIMULATION.md |
| Игровые данные `data/`, бой, марш, команды, время | docs/GAMEDATA.md |
| Экономика EU3-lite; каталог механик EU3 | docs/ECONOMY.md, docs/EU3.md |
| Рендер карты, модели, фигурки армий; UI | docs/RENDER.md, docs/UI.md |
| Исходники карты и её сборка (`tools/`); формат `world.mcmap` | docs/MAPBUILD.md, docs/MAPFORMAT.md |
| Правила для агента | `CLAUDE.md` |
