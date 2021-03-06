hook global BufCreate (.*/)?(kakrc|.*.kak) %{
    set buffer filetype kak
}

defhl kakrc
addhl -def-group kakrc regex \<(hook|rmhooks|defhl|addhl|rmhl|add|exec|eval|source|runtime|def|decl|echo|edit|set)\> 0:keyword
addhl -def-group kakrc regex \<(default|black|red|green|yellow|blue|magenta|cyan|white)\> 0:value
addhl -def-group kakrc regex (?<=\<hook)\h+((global|buffer|window)|(\S+))\h+(\S+)\h+(\H+) 2:attribute 3:error 4:identifier 5:string
addhl -def-group kakrc regex (?<=\<set)\h+((global|buffer|window)|(\S+))\h+(\S+)\h+(\S+) 2:attribute 3:error 4:identifier 5:value
addhl -def-group kakrc regex (?<=\<regex)\h+(\S+) 1:string
addhl -def-group kakrc regex (?<!\\)(["'])(?:\\\1|.)*?\1 0:string
addhl -def-group kakrc regex (^|\h)\#[^\n]*\n 0:comment
addhl -def-group kakrc region_ref '%sh\{' '\}' sh

hook global WinSetOption filetype=kak %{ addhl ref kakrc }
hook global WinSetOption filetype=(?!kak).* %{ rmhl kakrc }
