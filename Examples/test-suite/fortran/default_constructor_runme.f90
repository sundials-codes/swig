! File : default_constructor_runme.f90

program default_constructor_runme
  use default_constructor
  use ISO_C_BINDING
  implicit none
  type(G) :: ginstance
  ginstance = G()
  ! call ginstance%release()

  ! Call a static method
  call ginstance%destroy(ginstance)
end program


