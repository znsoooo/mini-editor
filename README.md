# Mini Editor

(This project is not yet completed, the following is the development plan)

Mini-editor is a text editor suitable for running in the console, using only ASCII character control, you can complete text editing operations without taking your hands off the keyboard.

There are two modes in the mini-editor, edit mode and command mode, which can be switched by pressing the Esc key. After starting the mini-editor, it defaults to the editing mode, and after pressing the Esc key to enter the command mode, press the Esc key again to return to the editing mode.

The mini-editor wants to bring the same operating experience as other traditional text editors, so it uses an almost identical hotkey design, but does not use the Ctrl key as a key combination. For example, if you use Ctrl-Z to indicate undo on other editors, you can press the Z key in the mini-editor command mode to do the same. Pressing Shift-Z produces an effect similar to Ctrl-Shift-Z.

Mini-editor hopes to be able to use it easily in console. In order to avoid affecting the hotkey settings of the console itself, the hotkeys of the mini-editor do not use Ctrl and Alt as a key combination, and the keys required are only 0-9/A-Z/Shift/Esc/Enter/Backspace. All of these keys have ASCII code values, so the mini-editor will have strong portability compatibility.

Since the hotkey design of the mini-editor is almost the same as that of the text editor in the graphical interface, if you need to use the text editor in the graphical interface, it is better to use a traditional text editor. There are no plans to create a graphical version of the mini-editor.

## Feature Introductions

Install mini-editor (not implemented):

> pip install mini-editor

Using mini-editor to create a blank document :

> $ mini

Using mini-editor to open a file:

> $ mini FILENAME

## Operation Instructions

When you open a new file, mini-editor is in edit mode, press Esc to enter command mode. In command mode, you can moving cursor, editing text, or operating file. If you want to exit command mode, press Esc again.

### Cursor Operations

```
[   ] [ ] [ ] [ ] [ ] [ ] [ ] [ ] [I] [ ] [ ]
[    ] [ ] [ ] [ ] [ ] [G] [ ] [J] [K] [L]
[Shift] [ ] [ ] [ ] [ ] [ ] [ ] [ ]
```

- I/J/K/L: Move cursor
- Shift-I/J/K/L: Select text

### Basic Operations

```
[ ] [ ] [ ] [ ] [ ] [ ] [ ] [ ] [ ] [ ]
 [A] [ ] [ ] [ ] [ ] [ ] [ ] [ ] [ ]
  [Z] [X] [C] [V] [ ] [ ] [ ]
```

- A: Select all text
- X: Cut selected text
- C: Copy selected text
- V: Paste text
- Z: Undo action
- Shift-Z: Redo action

### Search and Locate

```
[ ] [ ] [ ] [ ] [ ] [ ] [ ] [ ] [ ] [ ]
 [ ] [ ] [ ] [F] [G] [ ] [ ] [ ] [ ]
  [ ] [ ] [ ] [ ] [ ] [ ] [ ]
```

- F: Find in whole text
- Shift-F: Reverse find in whole text
- G: Jump to position

### Text Operations

```
[ ] [ ] [ ] [ ] [ ] [ ] [U] [ ] [ ] [ ]
 [ ] [ ] [D] [ ] [ ] [ ] [ ] [ ] [ ]
  [ ] [ ] [ ] [ ] [ ] [ ] [ ]
```

- U: Convert selected text to lowercase
- Shift-U: Convert selected text to uppercase
- D: Duplicate the current line
- Shift-D: Delete the current line

### File Operations

```
[ ] [W] [ ] [ ] [ ] [ ] [ ] [ ] [ ] [ ]
 [ ] [S] [ ] [ ] [ ] [ ] [ ] [ ] [ ]
  [ ] [ ] [ ] [ ] [ ] [ ] [ ]
```

- S: Save file
- Shift-S: Save as another filename
- W: Close file
