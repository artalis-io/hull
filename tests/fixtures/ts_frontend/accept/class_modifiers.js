// Class and member modifiers, abstract and declare members, index signatures, implements.
         class Shape extends Root                                    {
                            
                                    
                                 
                           
                                   
           name         = "shape";
            secret = 1;
                       id = 2;
           static          count         = 0;
    static                   version = 3;
             toString()         { return this.name; }
                                               
           get size()         { return 1; }
            set size(v        ) {}
           async load()                {}
            static helper() {}
           constructor(name        ) { this.name = name; }
}
export          class Base {}
class Impl extends Shape                     {
    area()         { return 0; }
    sides = 4;
    draw()       {}
}
const Anon = class                     { draw() {} };
class Names {
    public = 1;
    private;
    readonly
    x = 2;
    static
    y = 3;
    declare        ;
    abstract         ;
    static readonly         ;
}
class Spaced {
    a = 1;
                      
            c = 2
    d = 3
            e = 4;
}
