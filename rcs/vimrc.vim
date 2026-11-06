" Switch mode:
"   Esc/Tab: SwitchMode
" 
" Move cursor:
"   ijkl: MoveCursor
"   IJKL: SelectText
"   a: SelectAll
"   []: PrevWord/NextWord
"   -=: PageUp/PageDown
" 
" Basic function:
"   xcv: Cut/Copy/Paste
"   zZ: Undo/Redo
"   sq: Save/Quit
" 
" Advance function:
"   tT: Indent/Dedent
"   uU: LowerCase/UpperCase
"   dD: DuplicateLines/DeleteLines
" 
" Other function:
"   b: Block selecting
"   w: Window command


" Global settings:

set number
set autoindent
set expandtab
set tabstop=4
set shiftwidth=4


" Special keys behavior:

set whichwrap+=h,l,b,s,<,>,[,]
set backspace=indent,eol,start
set virtualedit+=block,onemore

noremap <BS> X
noremap <CR> i<CR><Esc>
noremap <Space> i<Space><Esc>

nnoremap <Tab> i
inoremap <Tab> <Esc>l
nnoremap <Esc> i
inoremap <Esc> <Esc>l

nnoremap b <C-V>


" Window functions:

noremap q :q<CR>
noremap Q :q!<CR>
noremap s :w<CR>
noremap S :wq<CR>


" Cursor functions:

noremap a ggvG$

nnoremap i k
nnoremap k j
nnoremap j h
nnoremap l l
nnoremap I vk
nnoremap K vj
nnoremap J vh
nnoremap L v
vnoremap i <Esc>k
vnoremap k <Esc>j
vnoremap j <Esc>h
vnoremap l <Esc>l
vnoremap I k
vnoremap K j
vnoremap J h
vnoremap L l

nnoremap [ b
nnoremap ] w
nnoremap { vb
nnoremap } vw
vnoremap [ <Esc>b
vnoremap ] <Esc>w
vnoremap { b
vnoremap } w

nnoremap - <C-U>
nnoremap = <C-D>
nnoremap _ v<C-U>
nnoremap + v<C-D>
vnoremap - <Esc><C-U>
vnoremap = <Esc><C-D>
vnoremap _ <C-U>
vnoremap + <C-D>


" Basic functions:

noremap z u
noremap Z <C-R>

nnoremap c yy
nnoremap v Pl
vnoremap x x
vnoremap c y
vnoremap v Pl


" Advance functions:

nnoremap t >>
nnoremap T <<
vnoremap t >
vnoremap T <

noremap d yyP
noremap D dd

nnoremap u guw
nnoremap U gUw
vnoremap u gu
vnoremap U gU
